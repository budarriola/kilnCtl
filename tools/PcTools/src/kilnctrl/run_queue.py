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
import itertools
import json
import logging
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, Optional, Sequence

from kilnctrl import capability_preflight, ramp_assist_http_client, zones_http_client
from kilnctrl import profile_stabilization as ps

log = logging.getLogger(__name__)

DEFAULT_HTTP_TIMEOUT_S = 5.0
DEFAULT_POLL_INTERVAL_S = 5.0
DEFAULT_RESTED_TOL_C = 1.0
DEFAULT_RESTED_TIMEOUT_S = 3600.0
DEFAULT_COOLDOWN_S = 600.0

#: how close the SECOND arm of a matched A/B pair (``--pair-consecutive``)
#: must start to the FIRST arm's own actual start temperature, per zone,
#: before the queue will fire it. This exists because :func:`is_rested`
#: alone does NOT guarantee two arms are comparable to each other -- it only
#: guarantees each arm, independently, reads no more than
#: ``DEFAULT_RESTED_TOL_C`` above its OWN cold junction. The cold junction
#: itself drifts upward run over run in a back-to-back campaign (confirmed
#: from the 2026-08-31 six-firing A/B campaign's real captures: cooldowns
#: consistently stopped within ~0.97-1.00C of the *current, already-elevated*
#: cold junction, never against a fixed baseline -- see
#: logs/coupling/ab_campaign_report.md sec 3 and the root-cause note this
#: constant's introduction added there), so a queue with no pairwise check
#: can -- and, live, did -- alternate old/new arms across a monotonically
#: warming baseline and call every arm "rested".
#:
#: 2026-09-03 REVISED to 1.5C (was 0.8C). The 0.8C value above was picked to
#: clear three LIVE pairs with margin, but never checked against what this
#: rig can physically deliver -- and live use found the gap: a paired arm B
#: held for 75 minutes against a 0.8C gate and still could not match. The
#: real constraint is passive cooling itself, not thermocouple noise: a
#: profile-7 cooldown was tracked to its own asymptote and it does not
#: approach the PREVIOUS session's start temperature at all -- it flattens
#: 1.4-1.5C above it and stays there (measured 16:30 vs 16:58, chamber
#: 29.83/29.95/29.99C -> 29.88/29.96/30.04C, i.e. <0.1C of further progress
#: in 28 minutes, while the cold junctions kept climbing 30.56->30.91 /
#: 30.75->31.05 / 30.88->31.19C). Approach to ambient is exponential, and an
#: early-phase rate extrapolated linearly is what made 0.8C look achievable
#: in the first place. A tolerance below the rig's own asymptotic floor is
#: not "tighter", it is unmeetable -- :func:`wait_until_paired_start` would
#: exhaust DEFAULT_PAIR_START_TIMEOUT_S on every single pair.
#:
#: 1.5C is the top of that measured 1.4-1.5C floor (not the bottom -- the
#: floor itself is a moving target across firings/ambient, so this sits at
#: its worse-observed edge rather than assuming the better one). Checked
#: against the owner's actual bar ("i dont care about sub 0.5C noise"), not
#: just against feasibility: this rig's own fitted start-temp sensitivity
#: (pid_ab_compare.fit_start_temp_sensitivity on the checked-in 6-run repeat
#: set, roughly 0.009-0.133C of iae_normalized_whole_c per 1C of start
#: delta -- see pid_ab_compare.py's CONFOUND_THRESHOLD_C derivation) predicts
#: that a pair sitting right at this 1.5C tolerance carries at most
#: 1.5 * 0.133 = 0.20C of confound in the most sensitive zone -- comfortably
#: under the 0.5C the owner has said is not actionable, and on the same
#: order as this rig's own measured whole-run IAE noise floors (z0 0.116 /
#: z1 0.077 / z2 0.147C). So 1.5C is simultaneously the loosest tolerance
#: this rig can actually achieve AND tight enough that the confound it
#: admits stays below both the actionable threshold and the noise floor --
#: it is not "give up and let anything through". See pid_ab_compare.py's
#: CONFOUND_THRESHOLD_C, which is now DERIVED FROM this constant (not a
#: separately-chosen 1.0C) specifically so a pair this gate accepts is never
#: turned around and refused by pid_ab_compare's own confound gate.
DEFAULT_PAIR_START_TOL_C = 1.5

#: how long wait_until_paired_start() will keep polling (beyond the normal
#: rested wait) for the second arm to cool down TO WITHIN
#: DEFAULT_PAIR_START_TOL_C of the first arm's start reading, before giving
#: up and raising loudly. Sized generously (2x DEFAULT_RESTED_TIMEOUT_S is
#: too much for a lever this narrow; 3600s = 1 hour is chosen instead)
#: because the live data shows the SAME natural cooldown gap (whichever a
#: full profile-7 run + its own cooldown loop already takes, ~50-70 minutes
#: in the 2026-08-31 campaign) sometimes already clears 0.5C on its own
#: (pair 2) and sometimes does not (pairs 1 and 3, which drifted
#: 1.2-1.7C over similarly-sized gaps) -- an extra hour of passive
#: convective cooling is the honest cost of comparability here, not a
#: number picked to make the wait feel short.
DEFAULT_PAIR_START_TIMEOUT_S = 3600.0

#: how long to wait, after a successful ``POST /api/profile_exec/start``, for
#: the executor to actually leave PROFILE_EXEC_IDLE (profile_executor.h:89 --
#: 0, the zero value) before giving up. This is the seam the live noise-floor
#: campaign fell into: run1 "finished" in 5.5s because the poll loop treated
#: an ``idle`` sample -- state 0, the same value the executor reports before
#: a run is ever started -- as proof the run was over, rather than as "not
#: started yet" or "stale read". profile_executor_run.c:805 sets
#: PROFILE_EXEC_RUNNING synchronously inside the call the HTTP handler makes
#: before it responds, so 30s of slack over a 5s poll interval is generous,
#: not tight.
DEFAULT_START_CONFIRM_TIMEOUT_S = 30.0

#: run-timeout policy: total_planned_s (GET /api/profile_plan's own field --
#: dashboard_http.c:1529, profile_feasibility_plan_curve()'s output) times
#: this multiplier, plus this flat margin. A profile that plans 50 minutes
#: is allowed up to 50*1.25 + 10 = 72.5 minutes of wall time before the
#: queue gives up and raises rather than silently believing a run that is
#: still ``running`` has finished.
DEFAULT_RUN_TIMEOUT_MULTIPLIER = 1.25
DEFAULT_RUN_TIMEOUT_MARGIN_S = 600.0

#: profile_exec state strings that mean "the executor has left idle and is
#: actively firing" (profile_executor.h's PROFILE_EXEC_RUNNING/PAUSED).
_ACTIVE_STATES = frozenset({"running", "paused"})

#: profile_exec state strings that mean "the run is over" (dashboard_http.c's
#: exec_state_name() / profile_executor.h's PROFILE_EXEC_* names, lower-cased
#: in the JSON body). Deliberately does NOT include "idle" -- PROFILE_EXEC_IDLE
#: is state 0, both "nothing has ever run" and "nothing is running right now",
#: and treating it as terminal is exactly the bug that let run_queue race
#: past a firing that had not even started yet (see
#: DEFAULT_START_CONFIRM_TIMEOUT_S above). A run that is genuinely idle after
#: having been confirmed RUNNING should never be observed again by this
#: module -- profile_executor_status.c never transitions RUNNING/PAUSED back
#: to IDLE, only to DONE or FAULTED.
_TERMINAL_STATES = frozenset({"done", "faulted"})


class RunQueueError(RuntimeError):
    """A safety refusal or an HTTP failure -- never silently skipped."""


class RunQueueFaultError(RunQueueError):
    """A running zone reported ``fault_guard != 0``. The queue must stop,
    not advance to the next entry -- a fault on entry N says nothing about
    whether entry N+1's preset/profile pairing is safe."""


class RunQueueClobberError(RunQueueError):
    """A capture path already exists and is non-empty. Raised instead of
    ever opening that path for write -- overwriting a completed (or
    partially captured) multi-hour firing's data is unrecoverable, and this
    module has already produced one live near-miss where only hand-picking
    a fresh base name avoided it. Applies unconditionally, resume or not:
    :func:`run_entry` checks this before it ever calls ``open(path, "w")``."""


class RunQueueStopFailedError(RunQueueError):
    """An already-started firing hit an error and the stop-on-exception
    path's own ``stop_profile()`` call ALSO failed (raised, or the state
    never cleared) -- the kiln may still be firing, unsupervised. Kept
    distinct from :class:`RunQueueError` so :func:`main` can print an
    unmissable banner and exit with a distinct code: an operator who reads
    only the final line must learn the kiln may still be lit, not just see
    whatever the ORIGINAL (unrelated) exception said."""


# --------------------------------------------------------------------------
# Pure safety checks -- no HTTP, fixture-testable.
# --------------------------------------------------------------------------

def is_rested(status_body: dict, tol_c: float = DEFAULT_RESTED_TOL_C) -> bool:
    """True iff every valid thermocouple channel in a ``GET /api/status``
    body is no more than ``tol_c`` ABOVE its own cold junction. A channel
    reporting ``valid: false`` or a null ``temp_c``/``cj_c`` (MAX31856.c
    leaves both NaN on a faulted/absent channel -- dashboard_http.c line
    ~461) is ignored rather than treated as "not rested": a dead channel
    should not block every other zone's queue forever, and the
    ceiling/fault checks below catch a genuinely unsafe start on their own
    terms.

    This is deliberately ONE-sided, not ``abs(temp_c - cj_c) > tol_c``. The
    MAX31856's on-board cold-junction sensor self-heats, so a genuinely
    cold, rested kiln reads BELOW its own cold junction -- confirmed live
    with the kiln cold: zone temps 26.91/26.78/26.63C against cold
    junctions 28.22/28.39/28.62C, i.e. 1.31-1.99C below CJ. A two-sided
    check with a 1.0C tolerance would fail that forever and refuse to ever
    start. A zone colder than its cold junction is rested; only a zone
    reading HOTTER than its cold junction by more than tol_c indicates
    residual heat."""
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
        if float(temp_c) - float(cj_c) > tol_c:
            return False
    return saw_any


def is_paired_start_matched(status_body: dict, reference_status: dict,
                             tol_c: float = DEFAULT_PAIR_START_TOL_C) -> bool:
    """True iff every valid, comparable thermocouple channel in
    ``status_body`` is within ``tol_c`` of the SAME-POSITION channel's
    ``temp_c`` in ``reference_status`` -- the FIRST arm of a matched pair's
    actual recorded start reading. Positional (zip over the ``channels``
    lists), the same convention :func:`is_rested` uses, rather than keyed by
    a ``channel``/``index`` field -- neither this module's fixtures nor a
    real ``GET /api/status`` body are required to carry one.

    This is a DIFFERENT question than :func:`is_rested`: that function asks
    "is this arm cold relative to ITS OWN cold junction". This asks "does
    this arm's actual temperature line up with the OTHER arm's actual
    temperature", which is what makes the two arms of an A/B pair a valid
    comparison at all (see ``pid_ab_compare.py``'s own 1.0C start-temperature
    confound gate, and DEFAULT_PAIR_START_TOL_C's docstring above for why a
    campaign that only checks :func:`is_rested` can still produce
    incomparable pairs).

    A channel invalid in EITHER body, or missing a ``temp_c``, is skipped
    (same "don't let one dead channel block forever" stance as
    :func:`is_rested`); if NO channel is comparable at all, this returns
    False -- absence of any real comparison is refused, never treated as a
    match by default."""
    ref_channels = reference_status.get("channels") or []
    cur_channels = status_body.get("channels") or []
    saw_any = False
    for ref_ch, cur_ch in zip(ref_channels, cur_channels):
        if not ref_ch.get("valid", False) or not cur_ch.get("valid", False):
            continue
        ref_t = ref_ch.get("temp_c")
        cur_t = cur_ch.get("temp_c")
        if ref_t is None or cur_t is None:
            continue
        saw_any = True
        if abs(float(cur_t) - float(ref_t)) > tol_c:
            return False
    return saw_any


def per_zone_start_deltas(status_body: dict, reference_status: dict) -> dict:
    """Per-channel-index ``abs(temp_c - reference temp_c)``, positional (same
    zip-over-channels convention as :func:`is_paired_start_matched`) -- the
    REPORTED counterpart to that gate. A channel invalid in either body, or
    missing ``temp_c``, is skipped (same "one dead channel does not block
    everything" stance used throughout this module). Deliberately never
    raises and never gates anything -- this exists so the actual per-zone
    drift is visible to a reader ALONGSIDE whatever :func:`is_paired_start_matched`
    decided, not only as a pass/fail behind ``pair_start_tol_c``."""
    ref_channels = reference_status.get("channels") or []
    cur_channels = status_body.get("channels") or []
    out: dict = {}
    for i, (ref_ch, cur_ch) in enumerate(zip(ref_channels, cur_channels)):
        if not ref_ch.get("valid", False) or not cur_ch.get("valid", False):
            continue
        ref_t = ref_ch.get("temp_c")
        cur_t = cur_ch.get("temp_c")
        if ref_t is None or cur_t is None:
            continue
        out[i] = abs(float(cur_t) - float(ref_t))
    return out


def check_no_fault(exec_body: dict) -> None:
    """Raise :class:`RunQueueFaultError` if any zone in a
    ``GET /api/profile_exec`` body has ``fault_guard != 0``."""
    for z in exec_body.get("zones", []):
        fg = int(z.get("fault_guard", 0))
        if fg != 0:
            raise RunQueueFaultError(
                f"zone {z.get('zone')}: fault_guard={fg} -- stopping the queue, "
                "not advancing to the next entry")


def check_not_faulted(exec_body: dict) -> None:
    """Raise :class:`RunQueueFaultError` if ``exec_body``'s own top-level
    ``state`` is ``faulted``. This is distinct from :func:`check_no_fault`:
    profile_executor.h:93 documents PROFILE_EXEC_FAULTED as covering "a
    GLOBAL thermal guard tripped (or every active zone individually
    faulted)" -- a global trip need not show up as any single zone's
    ``fault_guard`` entry, so relying on :func:`check_no_fault` alone would
    miss it. Both checks run on every poll."""
    if str(exec_body.get("state", "")).lower() == "faulted":
        raise RunQueueFaultError(
            f"profile_exec state=faulted (fault_guard={exec_body.get('fault_guard')}): "
            f"{exec_body.get('fault_reason') or '(no reason reported)'} -- stopping the "
            "queue, not advancing to the next entry")


def profile_total_planned_s(plan_body: dict) -> float:
    """``total_planned_s`` from a ``GET /api/profile_plan`` body --
    dashboard_http.c's own duration estimate for the profile
    (profile_feasibility_plan_curve()'s output), used to size the run-poll
    timeout. Raises :class:`RunQueueError` if the field is missing rather
    than silently falling back to an unbounded wait."""
    total = plan_body.get("total_planned_s")
    if total is None:
        raise RunQueueError(
            "profile_plan body has no total_planned_s -- cannot size a run timeout from "
            f"it: {plan_body!r}")
    return float(total)


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


def get_profile_detail(host: str, profile_id: int, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    """GET /api/profile?id=<n> -- the full profile including its segments
    (unlike get_profile_plan, which returns only the plan curve). Field
    names match profiles_http.c's profile_detail_get_handler() exactly:
    each segment carries seg_kind/target_c/ramp_c_per_hr/dwell_min/
    io_target/io_state/io_blocking/io_leave_on_at_end."""
    return _get_json(host, f"/api/profile?id={profile_id}", timeout)


def save_profile_segments(
    host: str, profile_id: int, name: str, zone_mask: int, segments: Sequence[dict],
    timeout: float = DEFAULT_HTTP_TIMEOUT_S,
) -> dict:
    """POST /api/profile, overwriting ``profile_id`` in place with
    ``segments`` (a list of the same per-segment dicts GET /api/profile
    returns -- see get_profile_detail). Field names match
    profiles_http.c's parse_profile_fields() exactly (seg{i}_kind/target/
    ramp/dwell for a ZONE_RAMP segment, seg{i}_io_target/io_state/
    io_blocking/io_leave_on for a RELAY_IO one). Overwrites IN PLACE (the
    same profile_id, not a new slot) -- ensure_stabilized_profile relies on
    this to be idempotent across repeated campaign runs against one board
    profile, rather than exhausting PROFILES_MAX_COUNT slots."""
    fields = {"id": str(profile_id), "name": name, "zone_mask": str(zone_mask),
              "seg_count": str(len(segments))}
    for i, seg in enumerate(segments):
        kind = int(seg.get("seg_kind", 0))
        fields[f"seg{i}_kind"] = str(kind)
        fields[f"seg{i}_dwell"] = str(int(seg.get("dwell_min", 0)))
        if kind == 1:  # PROFILE_SEG_KIND_RELAY_IO
            fields[f"seg{i}_io_target"] = str(int(seg.get("io_target", 0)))
            fields[f"seg{i}_io_state"] = str(int(seg.get("io_state", 0)))
            fields[f"seg{i}_io_blocking"] = str(int(seg.get("io_blocking", 1)))
            fields[f"seg{i}_io_leave_on"] = str(int(seg.get("io_leave_on_at_end", 0)))
        else:
            fields[f"seg{i}_target"] = str(float(seg.get("target_c", 0.0)))
            fields[f"seg{i}_ramp"] = str(float(seg.get("ramp_c_per_hr", 0.0)))
    body = _post_form(host, "/api/profile", fields, timeout)
    try:
        return json.loads(body)
    except Exception as exc:  # noqa: BLE001
        raise RunQueueError(f"POST /api/profile response was not JSON: {body!r}") from exc


def ensure_stabilized_profile(
    host: str, profile_id: int, timeout: float = DEFAULT_HTTP_TIMEOUT_S,
    target_c: float = ps.DEFAULT_STABILIZATION_TARGET_C,
    ramp_c_per_hr: float = ps.DEFAULT_STABILIZATION_RAMP_C_PER_HR,
    dwell_min: int = ps.DEFAULT_STABILIZATION_DWELL_MIN,
) -> bool:
    """TASK 1's runner-side half: make sure the board profile at
    ``profile_id`` begins with a stabilisation hold, prepending and saving
    one (IN PLACE -- same id) if it does not already. Returns True if the
    profile now has (or already had) the hold, i.e. the caller should
    expect the run's segment_index=0 to be the stabilisation hold and score
    from ``profile_stabilization.STABILIZATION_SEGMENT_INDEX``.

    IDEMPOTENT: if segment 0 already matches the stabilisation segment
    ``profile_stabilization.is_stabilization_segment`` would build for these
    parameters, this makes NO POST and returns True immediately -- a
    campaign that reuses one profile_id run after run (the common case)
    must not stack a second hold onto the first.

    Reuses :func:`profile_stabilization.prepend_stabilization_hold` for the
    actual prepend-and-validate step (over just the ZONE_RAMP-shaped
    target_c/ramp_c_per_hr/dwell_min fields), so its refusals (stabilisation
    target above 62C, an opening segment targeting below the stabilisation
    setpoint, exceeding PROFILE_MAX_SEGMENTS) apply unchanged and are never
    weakened here -- a RunQueueError wrapping the original ValueError
    propagates rather than silently starting an un-stabilised run.
    RELAY_IO segments (seg_kind=1) are passed through completely unmodified
    in both the check and the rebuilt list -- only their target_c/ramp/
    dwell would matter to prepend_stabilization_hold's opening-segment
    check, and a profile that opens on a RELAY_IO segment has no
    "temperature target" to compare against the stabilisation setpoint at
    all, so that check is skipped for a RELAY_IO opening segment."""
    detail = get_profile_detail(host, profile_id, timeout)
    segments = detail.get("segments", [])
    if not segments:
        raise RunQueueError(
            f"profile {profile_id} has no segments -- cannot ensure a stabilisation hold "
            "ahead of an empty profile")

    from types import SimpleNamespace
    first = segments[0]
    if ps.is_stabilization_segment(
        SimpleNamespace(**first), target_c=target_c, ramp_c_per_hr=ramp_c_per_hr,
        dwell_min=dwell_min,
    ):
        log.info("profile %d already begins with a stabilisation hold -- leaving it as is",
                  profile_id)
        return True

    # Only the ZONE_RAMP-shaped fields matter to prepend_stabilization_hold
    # (it never sees seg_kind/io_*); a RELAY_IO opening segment has no
    # target_c to compare, so treat it as having no ceiling on the check by
    # handing it the stabilisation target itself (0 delta -- neither
    # triggers nor evades the "opening segment below setpoint" refusal
    # incorrectly, since a RELAY_IO segment IS the profile's actual opening
    # step and this function has no temperature to judge it by).
    from .devices_profiles import ProfileSegment
    check_segments = [
        ProfileSegment(
            target_c=float(s.get("target_c", target_c)) if int(s.get("seg_kind", 0)) == 0 else target_c,
            ramp_c_per_hr=float(s.get("ramp_c_per_hr", 0.0)),
            dwell_min=int(s.get("dwell_min", 0)) or 1,
        )
        for s in segments
    ]
    try:
        ps.prepend_stabilization_hold(
            check_segments, target_c=target_c, ramp_c_per_hr=ramp_c_per_hr, dwell_min=dwell_min)
    except ValueError as exc:
        raise RunQueueError(
            f"refusing to prepend a stabilisation hold onto profile {profile_id}: {exc}") from exc

    stabilize_seg = {
        "seg_kind": 0, "target_c": target_c, "ramp_c_per_hr": ramp_c_per_hr,
        "dwell_min": dwell_min, "io_target": 0, "io_state": 0, "io_blocking": 0,
        "io_leave_on_at_end": 0,
    }
    new_segments = [stabilize_seg, *segments]
    if len(new_segments) > ps.PROFILE_MAX_SEGMENTS:
        raise RunQueueError(
            f"prepending a stabilisation hold onto profile {profile_id} would produce "
            f"{len(new_segments)} segments, exceeding PROFILE_MAX_SEGMENTS="
            f"{ps.PROFILE_MAX_SEGMENTS}")

    log.info("[profile %d] prepending a %.0fC/%.0fmin stabilisation hold (saved in place)",
              profile_id, target_c, dwell_min)
    result = save_profile_segments(
        host, profile_id, detail.get("name", ""), int(detail.get("zone_mask", 0)),
        new_segments, timeout)
    if not result.get("ok", False):
        raise RunQueueError(
            f"POST /api/profile refused while prepending a stabilisation hold onto "
            f"profile {profile_id}: {result!r}")
    return True


def start_profile(host: str, profile_id: int, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> dict:
    body = _post_form(host, "/api/profile_exec/start", {"id": str(profile_id)}, timeout)
    try:
        return json.loads(body)
    except Exception as exc:  # noqa: BLE001
        raise RunQueueError(f"POST /api/profile_exec/start response was not JSON: {body!r}") from exc


#: how long stop_profile() will keep re-polling GET /api/profile_exec for
#: the state to clear _ACTIVE_STATES after a stop POST, before giving up and
#: raising. A 200 response body is NOT proof the kiln actually stopped --
#: the firmware can (and does, on a malformed/rejected stop) answer
#: {"ok": false} with HTTP 200, which _post_form() treats identically to a
#: real ack since it only checks the status code and discards the body. The
#: entire safety value of the stop-on-exception path in run_entry() rests on
#: this call being real, so it must verify against the executor's own state,
#: not trust the POST response.
DEFAULT_STOP_CONFIRM_TIMEOUT_S = 30.0
DEFAULT_STOP_CONFIRM_POLL_INTERVAL_S = 1.0


def stop_profile(host: str, timeout: float = DEFAULT_HTTP_TIMEOUT_S,
                  confirm_timeout_s: float = DEFAULT_STOP_CONFIRM_TIMEOUT_S,
                  poll_interval_s: float = DEFAULT_STOP_CONFIRM_POLL_INTERVAL_S,
                  sleep: Callable[[float], None] = time.sleep,
                  now: Callable[[], float] = time.time) -> None:
    """POST /api/profile_exec/stop, then re-poll GET /api/profile_exec until
    the executor's own reported state is no longer 'running'/'paused'
    (_ACTIVE_STATES). Raises :class:`RunQueueError` if the stop POST itself
    fails, OR if the state has not cleared within ``confirm_timeout_s`` --
    in the latter case the kiln may still be firing and the caller must
    treat that as a failed stop, not a successful one."""
    body = _post_form(host, "/api/profile_exec/stop", {}, timeout)
    try:
        parsed = json.loads(body)
    except Exception:  # noqa: BLE001
        parsed = None
    if isinstance(parsed, dict) and parsed.get("ok") is False:
        raise RunQueueError(f"POST /api/profile_exec/stop was refused: {parsed!r}")

    start = now()
    while True:
        exec_body = _get_json(host, "/api/profile_exec", timeout)
        state = str(exec_body.get("state", "")).lower()
        if state not in _ACTIVE_STATES:
            return
        if (now() - start) > confirm_timeout_s:
            raise RunQueueError(
                f"POST /api/profile_exec/stop was acked but profile_exec state stayed "
                f"{state!r} for {confirm_timeout_s:.0f}s afterwards -- the kiln may still "
                "be firing")
        sleep(poll_interval_s)


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
    #: entries sharing the same non-None ``pair_key`` are treated as a
    #: matched A/B pair, in queue order: the FIRST entry with a given key
    #: records its actual start status; the SECOND (and only the second --
    #: the key is consumed once matched, see run_queue()) must start within
    #: ``RunQueueConfig.pair_start_tol_c`` of that recording (see
    #: :func:`wait_until_paired_start`) or the queue refuses rather than
    #: firing an arm that cannot be compared. ``None`` (the default) means
    #: "no pairing enforced" -- unchanged behaviour for entries that are not
    #: part of an A/B campaign.
    pair_key: Optional[str] = None
    #: OWNER DECISION (2026-09-03, TASK 1): True (the default) means
    #: ``run_entry`` ensures ``profile_id``'s board profile begins with a
    #: stabilisation hold (see :func:`ensure_stabilized_profile`) before
    #: starting it, and records ``STABILIZATION_SEGMENT_INDEX`` into this
    #: entry's captured log so ``pid_ab_compare.py`` scores past it
    #: automatically -- no operator has to remember either half. The
    #: rationale (PID_EXPANSION_PLAN.md sec 8): ~51 min/arm (5.6 min ramp to
    #: 48C + 45 min dwell) REPLACES the paired-start wait, and no future
    #: campaign can silently repeat the 2026-08-31 outcome (six firings, one
    #: usable pair) by forgetting to enforce comparable starts.
    #:
    #: False is the DOCUMENTED ESCAPE HATCH for a deliberate quick/
    #: unstabilised run -- set it explicitly (``--skip-stabilization-hold``
    #: on the CLI, or construct the entry with ``stabilize=False`` when
    #: scripting) when the ambient-confound protection genuinely is not
    #: wanted this run. It is never the accidental default.
    stabilize: bool = True


@dataclasses.dataclass
class RunQueueConfig:
    host: str
    poll_interval_s: float = DEFAULT_POLL_INTERVAL_S
    rested_tol_c: float = DEFAULT_RESTED_TOL_C
    rested_timeout_s: float = DEFAULT_RESTED_TIMEOUT_S
    cooldown_s: float = DEFAULT_COOLDOWN_S
    pair_start_tol_c: float = DEFAULT_PAIR_START_TOL_C
    pair_start_timeout_s: float = DEFAULT_PAIR_START_TIMEOUT_S
    http_timeout_s: float = DEFAULT_HTTP_TIMEOUT_S
    start_confirm_timeout_s: float = DEFAULT_START_CONFIRM_TIMEOUT_S
    run_timeout_multiplier: float = DEFAULT_RUN_TIMEOUT_MULTIPLIER
    run_timeout_margin_s: float = DEFAULT_RUN_TIMEOUT_MARGIN_S
    #: The stabilisation hold's setpoint (see ensure_stabilized_profile /
    #: profile_stabilization.py). Defaulted from profile_stabilization.py's
    #: own constant, not hardcoded here, so the two stay in sync -- but
    #: overridable per-fixture/per-profile-set: a fixture with different
    #: ambient headroom, or a campaign whose lowest scored profile opens
    #: below this bench's 40C default, needs a different number, and the
    #: refusal in prepend_stabilization_hold() (opening segment below the
    #: hold target) is exactly the signal that this needs adjusting rather
    #: than the profile being unusable.
    stabilization_target_c: float = ps.DEFAULT_STABILIZATION_TARGET_C
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


def wait_until_paired_start(cfg: RunQueueConfig, reference_status: dict,
                             tol_c: float = DEFAULT_PAIR_START_TOL_C,
                             deadline_s: Optional[float] = None) -> dict:
    """Block (polling ``GET /api/status``) until :func:`is_paired_start_matched`
    against ``reference_status`` is True, returning the matching status body.
    Raises :class:`RunQueueError` after ``pair_start_timeout_s`` (or
    ``deadline_s`` if given, for tests) -- FAILS LOUDLY rather than starting
    a second arm the campaign cannot compare against the first: an hour lost
    to this refusal is cheap next to the six hours the 2026-08-31 A/B
    campaign spent producing two-thirds unusable data because nothing
    enforced this.

    Deliberately separate from :func:`wait_until_rested`: the caller runs
    this ONLY after the ordinary rested wait already passed, and only for
    the second arm of a ``--pair-consecutive`` pair -- the first arm has
    nothing to match against yet."""
    timeout_s = DEFAULT_PAIR_START_TIMEOUT_S if deadline_s is None else deadline_s
    start = cfg.now()
    while True:
        status = get_status(cfg.host, cfg.http_timeout_s)
        if is_paired_start_matched(status, reference_status, tol_c):
            return status
        if cfg.now() - start > timeout_s:
            raise RunQueueError(
                f"refusing to start: zones did not settle within {tol_c}C of the paired "
                f"arm's start reading within {timeout_s:.0f}s -- starting now would produce "
                "an incomparable pair (see pid_ab_compare.py's own start-temperature "
                "confound gate). Wait longer by hand, or accept the campaign will not have "
                "this pair.")
        cfg.sleep(cfg.poll_interval_s)


def _capture_line(t: float, exec_body: dict, status_body: dict) -> str:
    return json.dumps({"t": t, "exec": exec_body, "status": status_body})


def _poll_capture_until(cfg: RunQueueConfig, fh, stop_predicate: Callable[[dict, dict], bool],
                         deadline_s: Optional[float] = None) -> None:
    """Poll exec+status once per ``poll_interval_s``, append one capture
    line each time, and check :func:`check_no_fault` / :func:`check_not_faulted`
    on every sample (a fault mid-run must stop the queue even though the run
    has not reached a terminal state yet). Returns when
    ``stop_predicate(exec, status)`` is True.

    ``deadline_s``, when given, is wall time (via ``cfg.now()``) measured
    from this call's own start: if the predicate has still not fired once
    that much time has elapsed, raises :class:`RunQueueError` -- "still
    running after the timeout" is an error, never a silent fall-through to
    the next queue entry (that silent fall-through, with no deadline at all,
    is exactly how run1 in the live noise-floor campaign started run2 on top
    of an active firing)."""
    start = cfg.now()
    while True:
        exec_body = get_exec(cfg.host, cfg.http_timeout_s)
        status_body = get_status(cfg.host, cfg.http_timeout_s)
        check_no_fault(exec_body)
        check_not_faulted(exec_body)
        fh.write(_capture_line(cfg.now(), exec_body, status_body) + "\n")
        fh.flush()
        if stop_predicate(exec_body, status_body):
            return
        if deadline_s is not None and (cfg.now() - start) > deadline_s:
            raise RunQueueError(
                f"profile_exec state={exec_body.get('state')!r} is still not terminal after "
                f"{deadline_s:.0f}s -- treating a run that outlives its own timeout as an "
                "error, not a completed run")
        cfg.sleep(cfg.poll_interval_s)


def _run_is_terminal(exec_body: dict, _status_body: dict) -> bool:
    return str(exec_body.get("state", "")).lower() in _TERMINAL_STATES


def _wait_until_run_active_or_terminal(cfg: RunQueueConfig, deadline_s: float) -> dict:
    """Poll ``GET /api/profile_exec`` (no capture, no sleep-then-check-first
    race) until the executor reports a state OTHER than idle -- i.e. it has
    actually started (RUNNING/PAUSED) or, for a legitimately near-instant
    firing, has already reached a terminal state. Raises
    :class:`RunQueueError` if it is still ``idle`` after ``deadline_s``.

    This is the fix for the root cause: a single poll immediately after
    ``POST /api/profile_exec/start`` returns is not proof of anything by
    itself, because PROFILE_EXEC_IDLE (state 0) is both "never started" and
    "not currently running" -- the same value. Confirming the state has
    actually moved to active (or terminal) before handing control to the
    run-capture loop means a stale/idle read can never again be mistaken
    for "the run is already over"."""
    start = cfg.now()
    while True:
        exec_body = get_exec(cfg.host, cfg.http_timeout_s)
        state = str(exec_body.get("state", "")).lower()
        if state in _ACTIVE_STATES or state in _TERMINAL_STATES:
            return exec_body
        if (cfg.now() - start) > deadline_s:
            raise RunQueueError(
                f"profile_exec state stayed {state!r} for {deadline_s:.0f}s after a "
                "successful POST /api/profile_exec/start response -- the executor never "
                "left idle")
        cfg.sleep(cfg.poll_interval_s)


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

    # PIN ramp_assist_enabled -- REQUIRED on every preset (config_presets.py's
    # _REQUIRED_TOP_FIELDS), for the identical reason config_presets.
    # apply_preset() pins it when it has a zones_host: this queue is exactly
    # the automated-experiment path the hazard is about -- a run that
    # inherited whatever the board happened to have left over from a
    # previous session (rather than an explicit pin) is the silent-
    # invalidation failure mode ramp_assist_cfg.h's header comment warns
    # about. Raises, same as the zones mismatch above, rather than starting a
    # firing whose ramp/dwell behaviour the caller did not actually pin.
    # pin_enabled() tolerates exactly one failure shape: enabled=False on
    # firmware built before /api/ramp_assist existed (trivially satisfied,
    # logs INFO, returns None). enabled=True against that same firmware is
    # still a hard failure -- RampAssistEndpointAbsentError, a subclass of
    # RampAssistHttpError, propagates uncaught -- and every other failure
    # (timeout, connection refused, HTTP 500, malformed body) is unaffected.
    ramp_assist_result = ramp_assist_http_client.pin_enabled(
        zones_host, bool(preset["ramp_assist_enabled"]), timeout=timeout, logger=log)
    if ramp_assist_result is not None and not ramp_assist_result.get("ok"):
        raise RunQueueError(
            f"POST /api/ramp_assist for preset {preset.get('name')!r} failed: {ramp_assist_result}")
    return result


def _resolve_preset(entry: QueueEntry, apply_preset_fn) -> dict:
    """Resolve the actual preset dict for ``entry``, following the same
    rule :func:`run_entry` already used inline before this was factored
    out: when the caller (a real CLI invocation) has NOT supplied an
    explicit ``apply_preset_fn``, ``entry.preset_name`` is a preset NAME
    resolved via ``config_presets.load_preset_data``; every test in this
    suite (and any other caller that injects ``apply_preset_fn`` directly)
    instead hands ``entry.preset_name`` the raw preset dict already, with
    no ``config_presets``/UART machinery in scope at all. Shared by
    :func:`run_entry` and :func:`_preflight_campaign` so preflight always
    sees the exact same preset data the run itself is about to apply."""
    if apply_preset_fn is None:
        from kilnctrl import config_presets
        return config_presets.load_preset_data(entry.preset_name)
    return entry.preset_name


def _preflight_campaign(entries: Sequence[QueueEntry], cfg: RunQueueConfig,
                         apply_preset_fn=None,
                         preflight_fn: "Optional[Callable]" = None) -> None:
    """Run a capability preflight for EVERY preset ``entries`` will use, all
    before :func:`run_queue` lets entry 0 begin -- see the incident this
    exists for in ``capability_preflight.py``'s own docstring: a preset
    field with no matching board endpoint used to surface as a Python
    traceback partway into an unattended run instead of a refusal up front.

    Runs ONCE per distinct preset, not once per entry -- the whole point is
    catching a gap before ANY firing starts, so a multi-entry queue's
    entry-3-only preset must be caught before entry 0's kiln is energised,
    not four hours later. Runs unconditionally, resume or not: see
    :func:`run_queue`'s docstring for why a resumed campaign is never
    exempted -- the board may have been reflashed, or DOWNgraded, between
    the original launch and the resume, and a capability that was present
    at launch and is missing now must abort exactly like a first-time gap
    would (this function has no memory of "checked before"; it only knows
    what the board answers right now).

    A board that cannot be reached at all is never reported as fine:
    ``PreflightReport.ok`` is False whenever ``BoardInfo.reachable`` is
    False, so ``preflight_fn`` (``capability_preflight.preflight_or_raise``
    by default) raises :class:`capability_preflight.PreflightFailed` for an
    unreachable board exactly as it does for a fatal capability gap --
    there being no capability data to be "benign" about is not the same as
    the run being safe to start."""
    if preflight_fn is None:
        preflight_fn = capability_preflight.preflight_or_raise

    checked = set()
    for entry in entries:
        preset = _resolve_preset(entry, apply_preset_fn)
        if apply_preset_fn is None:
            preset_name = entry.preset_name
        else:
            preset_name = (isinstance(preset, dict) and preset.get("name")) \
                or entry.label or entry.preset_name or "(unnamed)"
        # De-dup key: the resolved preset NAME, not identity -- two entries
        # that name the same preset (--repeat, or several queue entries
        # reusing one preset by name) are probed only once, whether the
        # caller resolves that name to a shared dict object (real
        # config_presets.load_preset_data, which does not cache) or a test
        # fixture hands two distinct-but-same-content dict objects for what
        # is conceptually "the same preset". Over-checking (a false miss on
        # this dedup) is harmless -- just an extra cheap probe; under-
        # checking (falsely treating two different presets as one) is not
        # possible here since the key IS the name a human/queue used to
        # refer to the preset.
        key = str(preset_name)
        if key in checked:
            continue
        checked.add(key)
        log.info("[preflight] checking preset %s capabilities against %s",
                  preset_name, cfg.host)
        preflight_fn(preset, cfg.host, zones_host=cfg.host, safety_host=None,
                     preset_name=str(preset_name))


def run_entry(entry: QueueEntry, cfg: RunQueueConfig, control=None,
              apply_preset_fn=None, pair_reference_status: Optional[dict] = None) -> dict:
    """Run one queue entry start to finish: apply preset, wait rested,
    safety-check, start, capture to ``entry.log_path`` through the run, then
    capture a cooldown tail into ``<log_path>.cooldown.jsonl``. Returns the
    ``GET /api/status`` body the entry actually started from -- ``run_queue``
    uses this to record the FIRST arm of a matched pair's start reading, so
    the SECOND arm can be checked against it.

    ``apply_preset_fn``, when omitted, is chosen from ``control``: when a
    ``ControlClient`` is given, ``config_presets.apply_preset`` (UART PID/
    model write + HTTP zones write); when ``control`` is ``None`` (the
    common case -- the kilnctrl MCP server holds the port),
    :func:`_apply_preset_http_only` (HTTP only, refuses up front if the
    preset needs a field only the UART CONTROL task can write). Passing
    ``apply_preset_fn`` explicitly overrides this selection entirely --
    tests use that to inject a fake with no ``config_presets``/UART
    machinery in scope at all.

    ``pair_reference_status``, when given (``QueueEntry.pair_key`` matched a
    prior entry -- see :func:`run_queue`), is the FIRST arm's actual start
    status: after the ordinary rested wait/reconfirm below, this entry ALSO
    blocks (:func:`wait_until_paired_start`) until its own reading lines up
    with it within ``cfg.pair_start_tol_c``, raising :class:`RunQueueError`
    rather than starting an arm the campaign will not be able to compare."""
    # TASK 1 (owner decision, 2026-09-03): stabilisation-hold-by-default.
    # entry.stabilize defaults True -- ensure the board profile has the
    # hold BEFORE anything else so every later step (the ceiling check
    # below, which re-fetches the plan; the actual start) sees the
    # already-stabilised profile. entry.stabilize=False is the documented
    # escape hatch for a deliberate quick/unstabilised run.
    stabilized_applied = False
    if entry.stabilize:
        stabilized_applied = ensure_stabilized_profile(
            cfg.host, entry.profile_id, cfg.http_timeout_s, target_c=cfg.stabilization_target_c)
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
    # have returned a while ago if the ceiling check above was slow. A
    # marginal drift here should send this entry back to waiting for rest,
    # not abort the whole remaining queue (a single entry's drift says
    # nothing about whether the other queued entries are safe) -- so retry
    # a bounded number of times before giving up. NOTE: each retry passes a
    # SHARED deadline (this loop's own start time + rested_timeout_s), not a
    # fresh full rested_timeout_s per call -- passing a fresh one each time
    # would let this loop spend up to
    # _PRESTART_RECONFIRM_ATTEMPTS * rested_timeout_s before giving up,
    # which is what the comment here used to (incorrectly) claim could not
    # happen. Sharing one deadline across every retry is what actually
    # bounds the total to rested_timeout_s.
    _PRESTART_RECONFIRM_ATTEMPTS = 3
    _prestart_reconfirm_start = cfg.now()
    status = get_status(cfg.host, cfg.http_timeout_s)
    for attempt in range(1, _PRESTART_RECONFIRM_ATTEMPTS + 1):
        if is_rested(status, cfg.rested_tol_c):
            break
        log.info("[%s] drifted out of rested tolerance before start (attempt %d/%d) -- "
                  "waiting for zones to rest again", entry.label, attempt,
                  _PRESTART_RECONFIRM_ATTEMPTS)
        if attempt == _PRESTART_RECONFIRM_ATTEMPTS:
            raise RunQueueError(
                f"zones drifted out of rested tolerance between the rested wait and the "
                f"start, and did not re-settle after {_PRESTART_RECONFIRM_ATTEMPTS} "
                f"retries -- refusing to start")
        remaining_s = max(
            0.0, cfg.rested_timeout_s - (cfg.now() - _prestart_reconfirm_start))
        wait_until_rested(cfg, deadline_s=remaining_s)
        status = get_status(cfg.host, cfg.http_timeout_s)

    # PAIRED-START GATE -- runs only for the second arm of a
    # ``--pair-consecutive`` pair (pair_reference_status is None otherwise).
    # This is the fix for the 2026-08-31 A/B campaign's root cause: every
    # arm above passed the ordinary "rested" check on its OWN terms, but
    # nothing checked the two arms of a pair against EACH OTHER, and a
    # back-to-back campaign's cold junction drifts upward run over run (see
    # DEFAULT_PAIR_START_TOL_C's docstring). Blocks, and re-reads ``status``
    # from the match so the value captured below (and handed back to
    # run_queue for the NEXT pair, if any) is the one that actually passed.
    if pair_reference_status is not None:
        log.info("[%s] waiting for start temperature to match its paired arm "
                  "(tol=%.1fC, timeout=%.0fs)", entry.label, cfg.pair_start_tol_c,
                  cfg.pair_start_timeout_s)
        status = wait_until_paired_start(
            cfg, pair_reference_status, tol_c=cfg.pair_start_tol_c,
            deadline_s=cfg.pair_start_timeout_s)
        # REPORT the actual per-zone start delta, matched or not, right next
        # to the gate that just passed -- the drift itself is a covariate a
        # reader should see (compared against pid_ab_compare.py's own
        # per-zone deltas later), not just a threshold this arm cleared.
        deltas = per_zone_start_deltas(status, pair_reference_status)
        log.info("[%s] paired-start matched -- per-zone start delta vs the first arm: %s",
                  entry.label, {i: round(d, 2) for i, d in deltas.items()})

    # Open (and thereby validate) the capture file BEFORE anything energizes
    # the kiln. Tonight's incident: the start POST landed, the heaters came
    # on, and only THEN did `open(entry.log_path, "w")` raise
    # FileNotFoundError on a bad path, leaving a live, uncaptured,
    # unsupervised firing. Opening first means an unwritable path refuses
    # the run instead of orphaning one.
    # REFUSE TO CLOBBER, unconditionally -- resume or not. A capture path
    # that already exists and is non-empty is either a completed run's data
    # or an interrupted partial capture that a resume caller was supposed to
    # discard explicitly (see _discard_partial_capture) before ever getting
    # here; either way, this function must never silently truncate it via
    # open(path, "w"). This is the fix for tonight's near-miss, where only
    # hand-picking a fresh base name avoided overwriting run 1's data.
    if os.path.exists(entry.log_path) and os.path.getsize(entry.log_path) > 0:
        raise RunQueueClobberError(
            f"refusing to start: capture file {entry.log_path!r} already exists and is "
            "non-empty -- overwriting it would destroy unrecoverable data from an earlier "
            "run. Move it aside, choose a different log_path, or (if this is a genuinely "
            "interrupted partial capture) let --resume discard it explicitly before "
            "re-running this entry.")

    log.info("[%s] opening capture file %s before starting", entry.label, entry.log_path)
    try:
        fh = open(entry.log_path, "w", encoding="utf-8")
    except OSError as exc:
        raise RunQueueError(
            f"cannot open capture file {entry.log_path!r} -- refusing to start the kiln "
            f"with nowhere to capture to: {exc}") from exc

    # META LINE -- TASK 1's producer/consumer agreement point. Written as
    # the very first line, before any poll row, so pid_ab_compare.py's
    # resolve_min_segment_index() can read it back without having to parse
    # (or care about the shape of) the rest of the capture. Records exactly
    # what this run did, not what it was asked to do: min_segment_index is
    # STABILIZATION_SEGMENT_INDEX only when a hold was actually confirmed in
    # place (stabilized_applied), never merely because entry.stabilize was
    # True -- see ensure_stabilized_profile's refusal paths above, which
    # raise before this point is ever reached, so in practice this is
    # always in sync with entry.stabilize once we get here, but the
    # variable it reads is the applied outcome, not the request.
    meta_line = json.dumps({"meta": {
        "stabilized": stabilized_applied,
        "min_segment_index": ps.STABILIZATION_SEGMENT_INDEX if stabilized_applied else 0,
    }})
    fh.write(meta_line + "\n")
    fh.flush()

    # started is set to True BEFORE the POST, not after it returns. The
    # start POST can be ACCEPTED by the board -- heaters energized -- while
    # the client sees a URLError/HTTPError/timeout raised out of
    # _post_form(). If `started` only flipped true on a *successful return*,
    # that path left `started` False, so the `except BaseException` handler
    # below never issued a stop_profile(), and the `finally` block then
    # os.remove()'d the capture file -- a live, unsupervised, uncaptured
    # firing. Setting it True first is pessimistic on purpose: a spurious
    # stop_profile() against a kiln that never actually started is harmless
    # (the board just answers "not running"); failing to stop one that did
    # start is not.
    started = False
    try:
        log.info("[%s] starting profile %d, capturing to %s", entry.label, entry.profile_id, entry.log_path)
        started = True
        result = start_profile(cfg.host, entry.profile_id, cfg.http_timeout_s)
        if not result.get("ok", False):
            # Unlike a URLError/HTTPError/timeout (ambiguous -- the board may
            # have accepted the POST anyway), an explicit {"ok": false} body
            # is the board itself confirming the profile did NOT start. That
            # is unambiguous, so it is safe to un-pessimize here: no stop is
            # needed and the empty capture file should still be cleaned up.
            started = False
            raise RunQueueError(f"POST /api/profile_exec/start refused: {result!r}")

        log.info("[%s] confirming the executor left idle (timeout=%.0fs)",
                  entry.label, cfg.start_confirm_timeout_s)
        _wait_until_run_active_or_terminal(cfg, cfg.start_confirm_timeout_s)

        run_timeout_s = profile_total_planned_s(plan) * cfg.run_timeout_multiplier + cfg.run_timeout_margin_s
        log.info("[%s] waiting for the run to reach a terminal state (timeout=%.0fs)",
                  entry.label, run_timeout_s)
        _poll_capture_until(cfg, fh, _run_is_terminal, deadline_s=run_timeout_s)
    except BaseException as original_exc:
        if started:
            # The start POST succeeded but something after it raised before
            # (or during) capture -- a crash here must not leave the kiln
            # firing with nothing watching it. Stop it, then propagate the
            # ORIGINAL exception (never swallowed).
            log.error("[%s] error after a successful start -- stopping the profile so a "
                       "crash cannot orphan a live firing", entry.label)
            try:
                stop_profile(cfg.host, cfg.http_timeout_s, sleep=cfg.sleep, now=cfg.now)
            except Exception as stop_exc:
                log.exception("[%s] failed to stop the profile while handling an earlier "
                               "error -- the kiln may still be running, check it by hand",
                               entry.label)
                # A failed stop after an already-started firing must never be
                # discoverable only by reading a full traceback: main()
                # reports whatever exception reaches it, and that is the
                # ORIGINAL error (by design -- never swallowed), so an
                # operator who reads only the final "queue stopped: ..."
                # line would see nothing at all about the failed stop. Wrap
                # in a distinct type so main() can print an unmissable
                # banner and exit with a distinct code.
                raise RunQueueStopFailedError(
                    f"[{entry.label}] the kiln may STILL BE FIRING: stop_profile failed "
                    f"({stop_exc}) while handling an earlier error ({original_exc})"
                ) from original_exc
        raise
    finally:
        fh.close()
        if not started:
            # The start POST never succeeded (refused, or failed before it
            # was even attempted) -- nothing was ever captured to this file,
            # so don't leave a stray empty capture file behind.
            try:
                os.remove(entry.log_path)
            except OSError:
                pass

    cooldown_path = entry.log_path + ".cooldown.jsonl"
    log.info("[%s] capturing cooldown to %s", entry.label, cooldown_path)
    cooldown_start = cfg.now()
    # Wrapped: an OSError here (bad path, disk full, permissions) must
    # surface as a RunQueueError like every other failure in this function
    # -- an uncaught OSError at this point would skip main()'s
    # `except RunQueueError` handler entirely (a bare traceback instead of
    # a clean exit code), AND -- because it happens after run_entry's own
    # work is otherwise done -- would leave this entry's state-file status
    # stuck at "in_progress" with a fully valid run capture already on
    # disk. See run_queue()'s RESUME COMPLETION RECOVERY note: that stuck
    # "in_progress" status is exactly the shape resume must not mistake for
    # a genuinely partial (and therefore discardable) capture.
    try:
        fh = open(cooldown_path, "w", encoding="utf-8")
    except OSError as exc:
        raise RunQueueError(
            f"[{entry.label}] cannot open cooldown capture file {cooldown_path!r}: {exc}"
        ) from exc
    with fh:
        _poll_capture_until(
            cfg, fh,
            lambda exec_body, status_body: (
                is_rested(status_body, cfg.rested_tol_c)
                or cfg.now() - cooldown_start > cfg.cooldown_s))

    return status


# --------------------------------------------------------------------------
# Campaign state file -- resumability.
#
# WHAT'S RECORDED: the queue definition (one dict per entry -- preset,
# profile_id, log_path, label) plus, per entry, a status
# ("pending"/"in_progress"/"completed") and the wall-clock time it finished.
# Written as plain indented JSON (see CampaignState's own field names) so an
# operator can read it at a glance without tooling -- requirement 5.
#
# DURABILITY: every update goes through _atomic_write_json, which writes to
# a sibling ``*.tmp`` file and calls os.replace() (atomic rename on both
# POSIX and Windows) rather than writing the real path in place. A crash
# mid-write leaves either the old, still-valid state file or a half-written
# ``*.tmp`` that nothing reads -- never a truncated, unparseable state file
# in the path a resume will look for.
#
# INTERRUPTED-ENTRY DECISION (requirement 2): a campaign process can die
# while an entry is "in_progress" -- e.g. mid-poll of _poll_capture_until.
# Per this module's own docstring, the firing itself is board-side and does
# NOT stop when the PC does, so that entry's capture file may be a genuine
# partial prefix of a firing that is either still running (if the board is
# still active -- see check_board_not_running_for_resume below, which
# refuses resume entirely in that case, so this path is only ever reached
# once the board is confirmed NOT running) or one that stopped on its own
# with nobody polling it. Either way, a half-captured firing is not a valid
# data point for a noise-floor measurement -- there is no way to tell from
# the state file alone whether the missing tail is "still recording" or
# "board finished this on its own, uncaptured, before the state file could
# be updated". Silently keeping a truncated capture and marking it
# "completed" would corrupt downstream analysis (log_analysis.py's
# multi-run tools assume a capture spans a full run + cooldown). So: on
# resume, once the board is confirmed idle, any "in_progress" entry's
# capture file (and cooldown sibling, if any) is DISCARDED and the entry is
# reset to "pending" so it re-runs from scratch, at the same log_path (never
# renumbered) -- consistent with the clobber refusal, since the file is
# removed before run_entry ever sees it again.
# --------------------------------------------------------------------------

#: v2 (was 1): added each entry's ``pair_start_status`` field, so a paired
#: entry's OWN recorded start reading survives a ``--resume`` -- v1 kept
#: ``pair_start_status`` (the module-level dict in :func:`run_queue`) purely
#: in-process, so a resumed campaign silently lost every pairing mid-way and
#: ran the next arm with NO start-temperature matching at all (found on the
#: bench, not in a review -- see :func:`_rebuild_pair_start_status`). Bumped
#: rather than adding the field with a ``.get(..., None)`` fallback on a
#: still-v1 file: a v1 state file predates this fix entirely, so guessing at
#: which of its completed entries were first arms would be exactly the kind
#: of silent reconstruction this fix exists to stop being silent about --
#: refusing to resume a v1 file is consistent with this module's existing
#: "refuse rather than guess" stance on an unknown version.
CAMPAIGN_STATE_VERSION = 2

#: per-process counter mixed into _atomic_write_json's temp filename so two
#: campaigns writing the same --state-file path never share one temp file.
_atomic_write_counter = itertools.count()


def _atomic_write_json(path: str, obj: dict) -> None:
    """Write ``obj`` as indented JSON to ``path`` via a sibling temp file
    plus ``os.replace`` -- atomic on both POSIX and Windows -- so a crash
    mid-write can never leave a truncated, unparseable state file behind.

    The temp path includes the PID and a per-process monotonic counter, not
    a fixed ``path + ".tmp"`` -- two campaigns (or two calls racing within
    one process, though this module never does that today) sharing one
    ``--state-file`` path would otherwise write the SAME temp file, and one
    process's half-written bytes could land under ``os.replace`` for the
    other's read.

    ``os.replace`` itself is wrapped: on Windows it raises ``PermissionError``
    if anything else holds ``path`` open (an editor, an AV scanner) at the
    exact moment of the rename. That is not a :class:`RunQueueError` by
    default, which means -- like the cooldown-open fix above -- it would
    escape ``main()``'s ``except RunQueueError`` as a bare traceback AND
    leave a completed entry's capture on disk with its state-file status
    never advanced past "in_progress". Wrapped here so it surfaces as a
    normal, catchable failure instead."""
    tmp_path = f"{path}.{os.getpid()}.{next(_atomic_write_counter)}.tmp"
    try:
        with open(tmp_path, "w", encoding="utf-8") as fh:
            json.dump(obj, fh, indent=2, sort_keys=False)
            fh.write("\n")
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp_path, path)
    except OSError as exc:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise RunQueueError(f"could not durably write campaign state to {path!r}: {exc}") from exc


def entry_to_dict(entry: QueueEntry) -> dict:
    return {"preset_name": entry.preset_name, "profile_id": entry.profile_id,
            "log_path": entry.log_path, "label": entry.label, "stabilize": entry.stabilize}


def entry_from_dict(d: dict) -> QueueEntry:
    return QueueEntry(preset_name=d["preset_name"], profile_id=d["profile_id"],
                       log_path=d["log_path"], label=d.get("label", ""),
                       stabilize=d.get("stabilize", True))


def new_campaign_state(entries: Sequence[QueueEntry], meta: Optional[dict] = None) -> dict:
    """Build a fresh campaign state dict for ``entries``, every one
    "pending". ``meta`` is informational only (host, tolerances, etc.) --
    purely for a human reading the file, never re-validated on resume."""
    now = time.time()
    return {
        "version": CAMPAIGN_STATE_VERSION,
        "created_at": now,
        "updated_at": now,
        "meta": meta or {},
        "progress": f"0/{len(entries)} completed",
        "entries": [
            {
                "index": i,
                "label": e.label,
                "preset_name": e.preset_name,
                "profile_id": e.profile_id,
                "log_path": e.log_path,
                "status": "pending",  # pending | in_progress | completed
                "completed_at": None,
                #: this entry's OWN recorded ``GET /api/status`` body, set
                #: only when this entry is the FIRST arm of a
                #: ``QueueEntry.pair_key`` pair and has completed -- see
                #: :func:`_rebuild_pair_start_status`. ``None`` for every
                #: unpaired entry, every not-yet-completed entry, and every
                #: SECOND arm (a second arm consumes the reference, it does
                #: not produce one).
                "pair_start_status": None,
            }
            for i, e in enumerate(entries)
        ],
    }


def load_campaign_state(state_path: str) -> dict:
    """Load and minimally validate a campaign state file. Raises
    :class:`RunQueueError` (never a bare ``JSONDecodeError``/``KeyError``/
    ``OSError``) for a missing file, a truncated/hand-edited unparseable
    file, a file that is valid JSON but not the expected shape, or a
    ``version`` this code does not know how to resume -- every one of
    those would otherwise escape ``main()``'s ``except RunQueueError``
    handler as a raw traceback instead of a clean, operator-facing exit."""
    try:
        with open(state_path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except OSError as exc:
        raise RunQueueError(f"cannot read campaign state file {state_path!r}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise RunQueueError(
            f"campaign state file {state_path!r} is not valid JSON -- truncated or "
            f"hand-edited? refusing to guess at a resume from a corrupt state file: {exc}"
        ) from exc
    if not isinstance(state, dict) or "entries" not in state or "version" not in state:
        raise RunQueueError(
            f"campaign state file {state_path!r} does not have the expected shape "
            "(missing 'version' and/or 'entries') -- refusing to resume from it")
    if state["version"] != CAMPAIGN_STATE_VERSION:
        raise RunQueueError(
            f"campaign state file {state_path!r} has version={state['version']!r}, this "
            f"code only knows how to resume version={CAMPAIGN_STATE_VERSION!r} -- refusing "
            "to guess at an incompatible state file's shape")
    return state


def save_campaign_state(state_path: str, state: dict) -> None:
    state["updated_at"] = time.time()
    done = sum(1 for e in state["entries"] if e["status"] == "completed")
    state["progress"] = f"{done}/{len(state['entries'])} completed"
    _atomic_write_json(state_path, state)


def _validate_resume_entries(state: dict, entries: Sequence[QueueEntry]) -> None:
    """Raise :class:`RunQueueError` if ``entries`` (the queue this
    invocation was given) does not line up index-for-index with the queue
    definition recorded in ``state``. Resuming against a different queue
    definition than the one that produced the state file would apply
    "completed"/"in_progress" status to the wrong entries -- refuse rather
    than guess."""
    saved = state.get("entries", [])
    if len(saved) != len(entries):
        raise RunQueueError(
            f"--resume: state file has {len(saved)} entries but this invocation supplied "
            f"{len(entries)} -- resume must be given the exact same queue definition as the "
            "original campaign so entry indices still line up")
    for i, (s, e) in enumerate(zip(saved, entries)):
        if (s["preset_name"], s["profile_id"], s["log_path"]) != (
                e.preset_name, e.profile_id, e.log_path):
            raise RunQueueError(
                f"--resume: entry {i} in the state file "
                f"({s['preset_name']!r}, {s['profile_id']!r}, {s['log_path']!r}) does not "
                f"match this invocation's entry "
                f"({e.preset_name!r}, {e.profile_id!r}, {e.log_path!r}) -- resume must reuse "
                "the identical queue definition as the original campaign")


def check_board_not_running_for_resume(host: str, timeout: float = DEFAULT_HTTP_TIMEOUT_S) -> None:
    """Raise :class:`RunQueueError` if ``GET /api/profile_exec`` reports an
    active state (running/paused) at the start of a ``--resume``.

    DECISION (requirement 4): REFUSE rather than adopt/supervise. The
    reasons an active firing might be observed here are genuinely
    ambiguous from the PC side alone -- it could be the interrupted
    in-progress entry, still going because profile_executor runs from the
    board's own flash and does not stop just because the PC died; or it
    could be an unrelated firing started some other way entirely. This
    module has no run-identifier from the board to distinguish those cases,
    so "adopt it" would mean guessing which queue entry (if any) it belongs
    to and resuming capture into that entry's log file mid-firing -- a
    capture with an unknown-length gap at the front is exactly the kind of
    invalid data point requirement 2 exists to avoid, just relocated to the
    front of the file instead of the back. Refusing is also what avoids the
    other failure mode this task is about: tonight's HTTP 409 came from
    starting a second profile on top of one already running, which killed a
    queue outright. Refusing here means resume never even attempts a
    start while the board's state is unknown-to-this-process."""
    exec_body = get_exec(host, timeout)
    state = str(exec_body.get("state", "")).lower()
    if state in _ACTIVE_STATES:
        raise RunQueueError(
            f"refusing to resume: profile_exec state={state!r} -- a profile is already "
            "running on the board. run_queue cannot tell whether this is the interrupted "
            "in-progress entry still finishing on its own (profile_executor runs from the "
            "board's own flash and does not stop just because the PC died) or an unrelated "
            "firing -- starting a second profile on top of it previously produced an HTTP "
            "409 and killed a queue. Wait for it to reach done/faulted (or stop it by hand "
            "via POST /api/profile_exec/stop) and then re-run --resume.")


def _capture_run_reached_terminal_state(log_path: str) -> bool:
    """True iff ``log_path`` (an entry's main run capture, NOT the cooldown
    sidecar) ends with a JSONL line whose ``exec.state`` is terminal
    (``_TERMINAL_STATES`` -- done/faulted). This is the signal that
    distinguishes a GENUINELY partial capture (process died mid-run --
    :func:`_poll_capture_until` never saw a terminal state, so the firing
    itself is an incomplete, invalid data point) from a capture whose run
    portion is fully complete and valid, but whose ENTRY was never marked
    "completed" because the process died afterwards -- during the cooldown
    capture, during ``save_campaign_state``, or anywhere else between
    ``run_entry`` finishing its real work and ``run_queue`` writing
    "completed". See the RESUME COMPLETION RECOVERY note where this is
    used: only a run that never reached a terminal state is discarded.

    Deliberately does NOT require the cooldown sidecar to exist or be
    complete: cooldown telemetry is supplementary (it exists to see the
    zones settle after a run, not to validate the run itself), and a crash
    between the run finishing and the cooldown file even being opened would
    otherwise cause this to (wrongly) discard a fully valid firing. Absence
    of proof is never treated as proof of completion in the OTHER
    direction, though: a missing, empty, or unparseable log_path returns
    False, same as :func:`is_rested`'s "no data is not rested" rule --
    when this function cannot find real evidence, the safe default is
    "not proven complete", i.e. still discardable."""
    try:
        last_line = None
        with open(log_path, "r", encoding="utf-8") as fh:
            for raw_line in fh:
                stripped = raw_line.strip()
                if stripped:
                    last_line = stripped
    except OSError:
        return False
    if not last_line:
        return False
    try:
        record = json.loads(last_line)
    except ValueError:
        return False
    state = str((record.get("exec") or {}).get("state", "")).lower()
    return state in _TERMINAL_STATES


def _first_capture_status(log_path: str) -> Optional[dict]:
    """Best-effort recovery of a capture's OWN first recorded ``status``
    body (``_capture_line``'s ``"status"`` field, first non-empty JSONL
    line) -- used ONLY when a resumed campaign is promoting an
    ``in_progress`` entry straight to ``completed`` (see
    :func:`_capture_run_reached_terminal_state`) and that entry never got a
    chance to persist its own ``pair_start_status`` before the process died
    (the crash landed between the run finishing and the state-file write
    that would have recorded it). The first capture line is polled
    immediately after the start POST, so it is a very close proxy for the
    actual start reading -- not byte-identical to what :func:`run_entry`
    would have recorded, but the same physical moment to within one poll
    interval. Returns ``None`` (never raises) for a missing, empty, or
    unparseable file/line -- absence of proof is not proof, and the caller
    (:func:`_rebuild_pair_start_status`) already refuses loudly rather than
    guess when this comes back empty and a later arm actually needs it."""
    try:
        with open(log_path, "r", encoding="utf-8") as fh:
            for raw_line in fh:
                stripped = raw_line.strip()
                if stripped:
                    try:
                        record = json.loads(stripped)
                    except ValueError:
                        return None
                    status = record.get("status")
                    return status if isinstance(status, dict) else None
    except OSError:
        return None
    return None


def _rebuild_pair_start_status(state: dict, entries: Sequence[QueueEntry]) -> dict:
    """Reconstruct the in-process ``pair_start_status`` dict (see
    :func:`run_queue`'s PAIRING comment) from a resumed state file's
    completed entries, replaying the exact same push-on-first-arm /
    pop-on-second-arm sequence :func:`run_queue`'s live loop uses -- so a
    ``--resume`` mid-campaign does not silently lose a pairing the way v1
    state files did (found on the bench: the second arm of a resumed pair
    started with NO start-temperature matching at all, and produced data
    that looked identical to a matched pair).

    Raises :class:`RunQueueError` if a completed FIRST arm's own start
    reading was never recorded (a v1-era gap this function cannot fully
    close, or a hand-edited state file) -- refusing to resume rather than
    silently letting a later arm of that pair run unpaired. This is the
    loud-refusal half of the fix: a wrong tolerance costs an hour, a
    silently-unpaired arm costs a firing that looks fine and is not."""
    pair_start_status: dict = {}
    for i, entry in enumerate(entries):
        if entry.pair_key is None:
            continue
        st_entry = state["entries"][i]
        if st_entry["status"] != "completed":
            continue
        if entry.pair_key in pair_start_status:
            # This entry is the SECOND arm -- consume/drop the reference,
            # mirroring run_queue()'s live pop-on-match behaviour so a
            # THIRD entry reusing this key starts a fresh pair.
            pair_start_status.pop(entry.pair_key, None)
        else:
            # This entry is the FIRST arm. Its own recorded start reading
            # must be recoverable, or a later (not-yet-run) second arm
            # would silently start with no pairing at all.
            saved_status = st_entry.get("pair_start_status")
            if saved_status is None:
                raise RunQueueError(
                    f"--resume: entry {i} ({entry.label or entry.preset_name!r}) is a "
                    f"completed first arm of pair {entry.pair_key!r} but its start "
                    "reading was not recorded in the state file (a v1-era state file, "
                    "or one hand-edited since) -- refusing to resume: a later arm of "
                    "this pair would silently start with NO start-temperature matching "
                    "at all, which fails silently and looks identical to a matched "
                    "pair in the resulting data. Start this pair over from scratch "
                    "(fresh --run, fresh log paths) instead.")
            pair_start_status[entry.pair_key] = saved_status
    return pair_start_status


def _discard_partial_capture(entry: QueueEntry) -> None:
    """Remove ``entry``'s capture file and its cooldown sibling, if either
    exists -- used only for an entry whose state-file status was
    "in_progress" at the start of a resume, once
    :func:`check_board_not_running_for_resume` has confirmed the board is
    NOT currently firing. See the INTERRUPTED-ENTRY DECISION note above:
    a partial capture is not a valid data point and must not be silently
    kept or (worse) appended to."""
    for path in (entry.log_path, entry.log_path + ".cooldown.jsonl"):
        try:
            os.remove(path)
            log.info("resume: discarded partial capture %s (interrupted run -- not a valid "
                      "data point, re-running the entry from scratch)", path)
        except OSError:
            pass


def run_queue(entries: Sequence[QueueEntry], cfg: RunQueueConfig, control=None,
              apply_preset_fn=None, state_path: Optional[str] = None,
              resume: bool = False, preflight_fn: "Optional[Callable]" = None) -> None:
    """Run every entry in order. Stops (re-raises) on the first
    :class:`RunQueueError` -- in particular a :class:`RunQueueFaultError`
    from mid-run -- rather than continuing to the next entry, since a fault
    or a refusal on entry N says nothing about whether N+1 is safe.

    Before ANY of that -- before the state file is even touched -- runs
    :func:`_preflight_campaign` against every distinct preset ``entries``
    will use (see that function's docstring): a missing capability must
    abort the whole campaign before entry 0's kiln is energised, not
    partway into an unattended run. This runs on every call, resume
    included -- see the RESUME COMPLETION RECOVERY note below for why a
    resumed campaign is never treated as "already proven safe".

    When ``state_path`` is given, a campaign state file is written before
    entry 0 starts and updated durably (write-then-rename) as each entry
    moves pending -> in_progress -> completed, recording the completed
    entries' capture paths. When ``resume`` is also True, the state file at
    ``state_path`` is loaded instead of created fresh: completed entries are
    skipped (never re-run, never re-numbered, never overwritten -- the
    clobber refusal in :func:`run_entry` would catch it even if this logic
    had a bug), and :func:`check_board_not_running_for_resume` runs first so
    a still-active board never gets a second start POST.

    RESUME COMPLETION RECOVERY: an "in_progress" entry found at resume time
    is NOT unconditionally treated as a crashed, discardable partial run.
    ``run_entry`` finishing its capture (reaching a terminal profile_exec
    state) and ``run_queue`` marking the entry "completed" in the state
    file are two separate steps with real time -- the cooldown capture,
    then a state-file write -- between them; a crash in that window
    (``open()`` failing on the cooldown path, ``save_campaign_state``
    itself raising, Ctrl-C, a power loss) leaves a COMPLETE, VALID run
    capture on disk while the state file still says "in_progress". The old
    unconditional-discard behaviour would delete that valid capture and
    re-fire the kiln for nothing. So: :func:`_capture_run_reached_terminal_state`
    is checked first -- if the run capture itself shows the firing reached
    done/faulted, the entry is promoted straight to "completed" (never
    discarded); only a capture that never reached a terminal state (the
    process genuinely died mid-run, which is not a valid noise-floor data
    point) is discarded and re-run from scratch, per the original
    INTERRUPTED-ENTRY DECISION."""
    _preflight_campaign(entries, cfg, apply_preset_fn=apply_preset_fn, preflight_fn=preflight_fn)

    # PAIRING: entries sharing a QueueEntry.pair_key are matched in queue
    # order. pair_start_status[key] holds the FIRST arm's actual start
    # reading once it has started; the key is POPPED the moment the second
    # arm consumes it (wait_until_paired_start matches it in run_entry), so
    # a third entry re-using the same key is treated as a fresh first arm
    # rather than silently matched against a stale reading. When a state
    # file is in use, every first arm's own recorded reading is ALSO
    # persisted into ``state["entries"][i]["pair_start_status"]`` (see
    # new_campaign_state) the moment it completes, and on --resume this
    # in-process dict is rebuilt from that persisted data
    # (:func:`_rebuild_pair_start_status`) rather than starting empty --
    # found on the bench: a v1 state file never persisted this, so a
    # resumed campaign silently lost every pairing mid-way and the next arm
    # started with NO start-temperature matching at all, indistinguishable
    # from a matched pair in the resulting data.
    pair_start_status: dict = {}

    state = None
    if state_path is not None:
        if resume:
            state = load_campaign_state(state_path)
            _validate_resume_entries(state, entries)
            check_board_not_running_for_resume(cfg.host, cfg.http_timeout_s)
            changed = False
            for i, entry in enumerate(entries):
                if state["entries"][i]["status"] == "in_progress":
                    if _capture_run_reached_terminal_state(entry.log_path):
                        log.info(
                            "[%s] resume: in_progress entry's run capture already reached "
                            "a terminal state -- the firing itself completed, only the "
                            "state-file update (or cooldown tail) was interrupted. "
                            "Promoting to completed rather than discarding valid data.",
                            entry.label or entry.preset_name)
                        state["entries"][i]["status"] = "completed"
                        if state["entries"][i].get("completed_at") is None:
                            state["entries"][i]["completed_at"] = time.time()
                        # This entry never got to persist its own
                        # pair_start_status (the crash landed before that
                        # write) -- best-effort recover it from the
                        # capture's own first line so a later arm of its
                        # pair does not need to refuse. See
                        # _first_capture_status for why this is a close
                        # proxy, not the byte-identical reading.
                        if (entry.pair_key is not None
                                and state["entries"][i].get("pair_start_status") is None):
                            recovered = _first_capture_status(entry.log_path)
                            if recovered is not None:
                                log.info(
                                    "[%s] resume: recovered pair_start_status from the "
                                    "capture's own first line (state-file write was "
                                    "interrupted before it recorded one)",
                                    entry.label or entry.preset_name)
                                state["entries"][i]["pair_start_status"] = recovered
                    else:
                        _discard_partial_capture(entry)
                        state["entries"][i]["status"] = "pending"
                    changed = True
            if changed:
                save_campaign_state(state_path, state)
            pair_start_status = _rebuild_pair_start_status(state, entries)
        else:
            if os.path.exists(state_path):
                raise RunQueueError(
                    f"state file {state_path!r} already exists -- pass --resume to continue "
                    "that campaign, or choose a different --state-file path so an existing "
                    "campaign's record is never silently overwritten")
            state = new_campaign_state(entries, meta={"host": cfg.host})
            save_campaign_state(state_path, state)

    for i, entry in enumerate(entries):
        if state is not None and state["entries"][i]["status"] == "completed":
            log.info("[%s] already completed -- skipping (resume)",
                      entry.label or entry.preset_name)
            continue
        if state is not None:
            state["entries"][i]["status"] = "in_progress"
            save_campaign_state(state_path, state)

        pair_reference_status = None
        if entry.pair_key is not None:
            pair_reference_status = pair_start_status.get(entry.pair_key)

        start_status = run_entry(entry, cfg, control=control, apply_preset_fn=apply_preset_fn,
                                  pair_reference_status=pair_reference_status)

        if entry.pair_key is not None:
            if pair_reference_status is None:
                # First arm of this pair -- remember its start reading for
                # whichever later entry shares this key. Also persisted into
                # the state file below (if one is in use) so a --resume
                # after this point can rebuild pair_start_status instead of
                # silently losing the pairing -- see
                # _rebuild_pair_start_status.
                pair_start_status[entry.pair_key] = start_status
                if state is not None:
                    state["entries"][i]["pair_start_status"] = start_status
            else:
                # Second arm consumed the reference -- drop it so a THIRD
                # entry reusing this key starts a fresh pair instead of
                # silently matching a stale reading.
                pair_start_status.pop(entry.pair_key, None)

        if state is not None:
            state["entries"][i]["status"] = "completed"
            state["entries"][i]["completed_at"] = cfg.now()
            save_campaign_state(state_path, state)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

#: a lone single-letter field immediately followed by a field starting with
#: "/" or "\\" is a Windows drive letter that ``raw.split(":")`` split apart
#: (``C:/Users/...`` -> ``["C", "/Users/..."]``), not two real fields of the
#: PRESET:PROFILE_ID:LOG_PATH[:LABEL] spec.
_DRIVE_LETTER_RE = re.compile(r"^[A-Za-z]$")


def _parse_run_arg(raw: str) -> QueueEntry:
    """Parse ``--run PRESET:PROFILE_ID:LOG_PATH[:LABEL]``.

    ``raw.split(":", 3)`` alone is wrong for a Windows absolute LOG_PATH: a
    spec like ``preset:7:C:/Users/.../run.jsonl:label`` splits on the drive
    letter's own colon, silently handing LOG_PATH the two characters "C" and
    shoving the real path into LABEL. That is exactly what happened live
    (``--run coupling_matrix_pre20260902:7:C:/Users/.../
    noise_floor_p7b.jsonl:noise_floor``) -- it did not error, it captured to
    a mangled ``C_run1`` file with the path as the label. Detect and
    re-merge a drive-letter split before applying the normal field split, so
    a Windows absolute path either parses correctly or this raises a clear
    error -- never a silent mangled path."""
    raw_parts = raw.split(":")
    merged = []
    i = 0
    while i < len(raw_parts):
        piece = raw_parts[i]
        if i > 0 and _DRIVE_LETTER_RE.match(piece) and i + 1 < len(raw_parts):
            nxt = raw_parts[i + 1]
            if nxt[:1] in ("/", "\\"):
                # C:/... or C:\... -- the absolute form. Unambiguous: re-merge.
                merged.append(piece + ":" + nxt)
                i += 2
                continue
            else:
                # C:foo -- the drive-RELATIVE form (relative to whatever the
                # process's current directory on drive C: happens to be).
                # This is the SAME silent-misparse hazard the absolute-path
                # fix above exists to kill: raw.split(":") still splits it
                # into a bare drive letter plus the rest, and there is no
                # slash to detect it by. Rather than guess at a meaning
                # (silently re-merge, or silently treat "C" as the drive and
                # the remainder as a path relative to an unknown cwd), refuse
                # outright and name the ambiguity -- a loud refusal here,
                # never a quiet mangled log_path.
                raise ValueError(
                    f"--run LOG_PATH looks like a Windows drive-relative path "
                    f"({piece!r}:{nxt!r} in {raw!r}) -- this form depends on the current "
                    f"directory on that drive and is refused rather than guessed at; use "
                    f"an absolute path (C:/... or C:\\\\...) instead")
        merged.append(piece)
        i += 1

    if len(merged) < 3:
        raise ValueError(
            f"--run must be PRESET:PROFILE_ID:LOG_PATH[:LABEL], got {raw!r}")
    preset, profile_id, log_path = merged[0], merged[1], merged[2]
    label = ":".join(merged[3:]) if len(merged) > 3 else preset
    if _DRIVE_LETTER_RE.match(log_path):
        # A LOG_PATH that is a single letter, after all merging above, is
        # exactly the historical silent-misparse signature (a real path got
        # split apart and only its drive letter landed in LOG_PATH, the rest
        # shifted into LABEL) -- refuse loudly rather than ever write a
        # capture file named "C".
        raise ValueError(
            f"--run LOG_PATH parsed as the single character {log_path!r} -- this is the "
            f"drive-letter silent-misparse signature, not a real path; got {raw!r} "
            f"(parsed fields: {merged!r})")
    try:
        profile_id_int = int(profile_id)
    except ValueError as exc:
        raise ValueError(
            f"--run PROFILE_ID must be an integer, got {profile_id!r} in {raw!r} -- if "
            f"LOG_PATH is a Windows absolute path, check it parsed as one field "
            f"(parsed fields: {merged!r})") from exc
    return QueueEntry(preset_name=preset, profile_id=profile_id_int, log_path=log_path, label=label)


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
            stabilize=entry.stabilize,
        ))
    return out


def main(argv: Optional[Sequence[str]] = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Run a queue of preset+profile firings, capturing HTTP telemetry to JSONL.")
    parser.add_argument("--host", required=True, help="board host/IP, e.g. 192.168.1.156")
    parser.add_argument(
        "--run", action="append", default=None, dest="runs",
        metavar="PRESET:PROFILE_ID:LOG_PATH[:LABEL]",
        help="one queued entry; repeat for multiple runs, run in order. Required unless "
             "--resume is given, in which case the queue definition is read back from "
             "--state-file instead.")
    parser.add_argument(
        "--state-file", default=None, metavar="PATH",
        help="campaign state file: records the queue definition and, per entry, "
             "pending/in_progress/completed + completed capture paths, updated durably "
             "(write-then-rename) after every entry. Without this flag the campaign is not "
             "resumable. With --resume, read from here instead of --run.")
    parser.add_argument(
        "--resume", action="store_true",
        help="resume a campaign from --state-file: skip completed entries, discard and "
             "re-run any entry left in_progress by a crash (see run_queue.py's "
             "INTERRUPTED-ENTRY DECISION docs), and refuse to start if the board is already "
             "running a profile.")
    parser.add_argument(
        "--repeat", type=int, default=1, metavar="N",
        help="repeat the single --run entry N times, same preset+profile, each waiting for "
             "its own genuinely rested start (run_entry always waits rested first -- this "
             "just runs it N times). Requires exactly one --run. log_path gets _run1.._runN "
             "inserted before its extension so no two runs can land in the same file -- the "
             "noise-floor campaign's turn-key form (PID_EXPANSION_PLAN.md SS3.3).")
    parser.add_argument(
        "--pair-consecutive", action="store_true",
        help="treat the queue as consecutive A/B pairs (entries 0&1, 2&3, 4&5, ...): the "
             "second entry of each pair will not start until its own temperature is within "
             "--pair-start-tol-c of the first entry's ACTUAL start reading, waiting up to "
             "--pair-start-timeout-s and refusing (RunQueueError) rather than starting an "
             "arm the campaign cannot compare against its partner. Fixes the confound the "
             "2026-08-31 A/B campaign hit: is_rested() alone only proves an arm is cold "
             "relative to ITS OWN cold junction, which drifts upward run over run in a "
             "back-to-back campaign, and says nothing about whether two arms line up with "
             "each other. Requires an even number of --run entries (or --state-file's saved "
             "queue, on --resume).")
    parser.add_argument(
        "--skip-stabilization-hold", action="store_true",
        help="OWNER DECISION (2026-09-03): the stabilisation hold is prepended onto every "
             "queued entry's profile by default (~51 min/arm: a few minutes' ramp to "
             "--stabilization-target-c + 45 min dwell, replacing the paired-start wait) and "
             "pid_ab_compare.py is told to score past it automatically. This is the "
             "DOCUMENTED ESCAPE HATCH for a deliberate quick/unstabilised run -- pass it "
             "explicitly; it is never the accidental default. Applies to every entry in this "
             "invocation (including --repeat and --pair-consecutive expansions).")
    parser.add_argument(
        "--stabilization-target-c", type=float, default=ps.DEFAULT_STABILIZATION_TARGET_C,
        help="setpoint (C) for the stabilisation hold prepended onto every entry's profile "
             "(default from profile_stabilization.py: %(default)sC, chosen for THIS bench's "
             "ambient range and profile set -- see that module's docstring for the margin "
             "arithmetic). Override per fixture/campaign: it must clear ambient with real "
             "margin AND sit below every scored profile's opening segment target, or "
             "ensure_stabilized_profile()/prepend_stabilization_hold() refuses that entry "
             "rather than silently reshaping the profile.")
    parser.add_argument("--poll-interval-s", type=float, default=DEFAULT_POLL_INTERVAL_S)
    parser.add_argument("--rested-tol-c", type=float, default=DEFAULT_RESTED_TOL_C)
    parser.add_argument("--rested-timeout-s", type=float, default=DEFAULT_RESTED_TIMEOUT_S)
    parser.add_argument("--cooldown-s", type=float, default=DEFAULT_COOLDOWN_S)
    parser.add_argument("--pair-start-tol-c", type=float, default=DEFAULT_PAIR_START_TOL_C)
    parser.add_argument("--pair-start-timeout-s", type=float, default=DEFAULT_PAIR_START_TIMEOUT_S)
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

    if args.resume:
        if not args.state_file:
            parser.error("--resume requires --state-file")
        state = load_campaign_state(args.state_file)
        entries = [entry_from_dict(e) for e in state["entries"]]
        if args.runs:
            log.info("--resume given alongside --run -- the queue definition is taken from "
                      "%s; --run is only cross-checked, not re-applied", args.state_file)
            entries_from_args = [_parse_run_arg(r) for r in args.runs]
            if args.repeat != 1:
                if len(entries_from_args) != 1:
                    parser.error("--repeat requires exactly one --run entry")
                entries_from_args = expand_repeat(entries_from_args[0], args.repeat)
            _validate_resume_entries(state, entries_from_args)
    else:
        if not args.runs:
            parser.error("--run is required unless --resume is given")
        entries = [_parse_run_arg(r) for r in args.runs]
        if args.repeat != 1:
            if len(entries) != 1:
                parser.error("--repeat requires exactly one --run entry")
            entries = expand_repeat(entries[0], args.repeat)

    if args.skip_stabilization_hold:
        entries = [dataclasses.replace(e, stabilize=False) for e in entries]

    if args.pair_consecutive:
        if len(entries) % 2 != 0:
            parser.error(
                f"--pair-consecutive requires an even number of queue entries, got "
                f"{len(entries)}")
        entries = list(entries)
        for i in range(0, len(entries), 2):
            key = f"pair{i // 2}"
            entries[i] = dataclasses.replace(entries[i], pair_key=key)
            entries[i + 1] = dataclasses.replace(entries[i + 1], pair_key=key)

    cfg = RunQueueConfig(
        host=args.host, poll_interval_s=args.poll_interval_s, rested_tol_c=args.rested_tol_c,
        rested_timeout_s=args.rested_timeout_s, cooldown_s=args.cooldown_s,
        pair_start_tol_c=args.pair_start_tol_c, pair_start_timeout_s=args.pair_start_timeout_s,
        stabilization_target_c=args.stabilization_target_c)

    control = None
    if args.serial_port:
        from kilnctrl.serial_link import UartLink
        from kilnctrl.control import ControlClient
        link = UartLink(args.serial_port)
        control = ControlClient(link)

    try:
        run_queue(entries, cfg, control=control, state_path=args.state_file, resume=args.resume)
    except capability_preflight.PreflightFailed as exc:
        # str(exc) is already PreflightReport.describe() -- the full,
        # operator-facing report (which capability, why fatal, the remedy).
        # Caught explicitly (a distinct exit code, same pattern as the
        # stop-failed banner below) rather than letting this fall through
        # to a bare traceback, even though PreflightFailed is not a
        # RunQueueError subclass.
        log.error("campaign refused before anything started:\n%s", exc)
        return 3
    except RunQueueStopFailedError as exc:
        log.error("queue stopped: %s", exc)
        log.error("!" * 70)
        log.error("!! KILN MAY STILL BE FIRING -- STOP-ON-ERROR FAILED. CHECK BY HAND. !!")
        log.error("!" * 70)
        return 2
    except RunQueueError as exc:
        log.error("queue stopped: %s", exc)
        return 1
    finally:
        if control is not None:
            control.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
