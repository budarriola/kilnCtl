"""Wave 0's only implemented cases: the read-only `smoke` suite.

Each `_case_XXX(ctx)` function does the fetching (calling the Python
functions the sibling kilnctrl MCP tools wrap -- never a second MCP server,
never hardware directly, per the plan doc §2.2) and hands the result to a
pure judge function from judgments.py. `ctx` is the dict the runner builds
per-run: `host` (board LAN/AP address) and a `srv` module reference (kilnctrl.mcp_server,
imported lazily so this module stays importable -- and unit-testable with
srv mocked out -- with no board attached).

Every _case_ function is intentionally thin: fetch, then delegate the
actual judgment to judgments.py so that logic is unit-tested in isolation
(tools/PcTools/tests/test_bench_test_judgments.py) without a live board.
"""
from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.request
from typing import Any, Optional

from . import judgments as J
from .. import http_auth
from .registry import CaseResult, Verdict, get_case

#: safety_clear_trip() is fire-and-forget (devices_safety.py) and
#: safety_get_diag() answers from the ESP's cache of the Pico's last DIAG
#: push, which only refreshes every LINK_DIAG_TX_PERIOD_MS = 2000 ms
#: (firmware/SaftyFW/src/tasks/link_task.c:209) -- a single immediate
#: read-back after clearing a trip can see a stale trip_reason even though
#: the clear itself succeeded. The diag context age is observed up to ~3.5 s,
#: so a 3.0 s window FAILed a working clear (HP-07, 2026-10-03, elapsed
#: 3.04 s). 10.0 s is > 2x that worst-case context age plus the 2 s DIAG
#: period. Shared by HP-07 (cases_heat.py) and FL-11 (cases_fl.py).
_TRIP_CLEAR_POLL_TIMEOUT_S = 10.0
_TRIP_CLEAR_POLL_INTERVAL_S = 0.3


def wait_for_trip_clear(
    ctx: dict, srv, timeout_s: float = _TRIP_CLEAR_POLL_TIMEOUT_S,
    interval_s: float = _TRIP_CLEAR_POLL_INTERVAL_S,
    observed: "Optional[dict]" = None,
) -> "tuple[Optional[int], float]":
    """Calls `srv.safety_clear_trip()` (fire-and-forget) then polls
    `srv.safety_get_diag()` until `trip_reason` reads 0 or `timeout_s`
    elapses, using `ctx`'s `_sleep`/`_now` seam so fake-board unit tests
    stay fast. Returns `(last_trip_reason_seen, elapsed_seconds)` -- on a
    timeout, the caller has both the last reason actually observed and how
    long it waited, for an informative failure message.

    When `observed` is given, also records `clear_ack` (safety_clear_trip()'s
    return value, as a string) and `trip_reason_timeline` (a list of
    `(elapsed_s rounded to 2 dp, trip_reason)` per poll) into it."""
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    ack = srv.safety_clear_trip()
    start = now()
    timeline: list = []
    if observed is not None:
        observed["clear_ack"] = None if ack is None else str(ack)
        observed["trip_reason_timeline"] = timeline
    last_reason: Optional[int] = J.parse_trip_reason(srv.safety_get_diag())
    timeline.append((round(now() - start, 2), last_reason))
    while last_reason != 0:
        elapsed = now() - start
        if elapsed >= timeout_s:
            return last_reason, elapsed
        sleep(interval_s)
        last_reason = J.parse_trip_reason(srv.safety_get_diag())
        timeline.append((round(now() - start, 2), last_reason))
    return last_reason, now() - start


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/bench_test/ -> repo root is five levels
    up. Same convention as report.default_logs_root()."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", ".."))


def _srv(ctx: dict):
    srv = ctx.get("srv")
    if srv is None:
        from .. import mcp_server as srv  # local import: keeps this module importable with no board/MCP process
    return srv


def _http_get_json(host: str, path: str, timeout: float = 5.0) -> "tuple[Optional[int], Any]":
    """Minimal raw GET for routes with no existing typed client yet
    (plan §2.2's 'raw GET/POST ... through the existing helpers' escape
    hatch, for the handful of routes -- /api/coredump/info -- that predate
    one). SP-05 no longer belongs in this category: it was rewritten to be
    read-only via srv.safety_get_status() rather than a raw POST to
    /api/estop/verify, an admin write.

    Several routes this helper reads (``/api/coredump/info`` for FL-06,
    ``/api/saftyfw_stack_margin`` for SK-03, ``/api/ramp_assist`` for AT-01's
    precondition check) are ROUTE_TIER_ADMIN. Goes through
    ``kilnctrl.http_auth.urlopen`` -- the one seam every other admin-tier
    PcTools client authenticates through (see that module's docstring) --
    rather than a bare ``urllib.request.urlopen``, so a board with web auth
    enabled answers a 401 with a login-and-retry instead of failing every
    caller of this helper. Byte-for-byte identical to the old bare request
    when auth is disabled (http_auth's own contract)."""
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            status = resp.getcode()
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8", errors="replace") if exc.fp else ""
    except (urllib.error.URLError, OSError, http_auth.HttpAuthError) as exc:
        return None, str(exc)
    try:
        return status, json.loads(body) if body else {}
    except json.JSONDecodeError:
        return status, body


# ---------------------------------------------------------------------------
# ST-05 -- tree provenance. No board needed at all.
# ---------------------------------------------------------------------------

def _case_st05(ctx: dict) -> CaseResult:
    from .. import flash_provenance

    repo_root = ctx.get("repo_root")
    state = flash_provenance.capture_tree_state(repo_root)
    ctx["_st05_head"] = state.head
    return J.judge_tree_provenance(state.dirty_files, state.sensitive_files)


# ---------------------------------------------------------------------------
# FL-01/02 -- partition table.
# ---------------------------------------------------------------------------

def _case_fl01(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    report_text = srv.debug_check_partition_table(host=ctx.get("host"))
    ctx["_fl01_partitions"] = report_text  # for WEB-DIAG-03
    return J.judge_partition_table_match(report_text)


def _case_fl02(ctx: dict) -> CaseResult:
    from .. import partition_http_client

    host = ctx["host"]
    try:
        data = partition_http_client.get_partitions(host)
    except Exception as exc:  # noqa: BLE001 - report as a FAIL, not a crash
        return CaseResult(Verdict.FAIL, reason=f"GET /api/partitions failed: {exc}", observed={})
    return J.judge_running_partition(data.get("running"))


def _case_fl03(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    text = srv.find_crash_elf(host=ctx.get("host"))
    return J.judge_archived_elf_matches(text)


def _case_fl04(ctx: dict) -> CaseResult:
    from .. import ota_http_client

    host = ctx["host"]
    try:
        data = ota_http_client.get_boot_guard_status(host)
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.FAIL, reason=f"GET /api/boot_guard failed: {exc}", observed={})
    return J.judge_boot_guard(data)


def _case_fl05(ctx: dict) -> CaseResult:
    from .. import partition_http_client

    host = ctx["host"]
    try:
        data = partition_http_client.get_partitions(host)
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.FAIL, reason=f"GET /api/partitions failed: {exc}", observed={})
    recovery_row = next((p for p in data.get("partitions", []) if p.get("label") == "recovery"), None)
    recovery_bin_size = None
    candidate = ctx.get("recovery_bin_path") or os.path.join(
        ctx.get("repo_root") or _repo_root(), "firmware", "KilnFW_recovery", "build", "recovery.bin"
    )
    if os.path.isfile(candidate):
        recovery_bin_size = os.path.getsize(candidate)
    return J.judge_recovery_image_sized(recovery_row, recovery_bin_size)


def _case_fl06(ctx: dict) -> CaseResult:
    host = ctx["host"]
    status, body = _http_get_json(host, "/api/coredump/info")
    if status is None:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/coredump/info: {body}", observed={})
    return J.judge_coredump_readable(status, body)


def _case_fl07(ctx: dict) -> CaseResult:
    """FL-07 never FAILs (plan §7 owner decision 6): a route error is
    recorded as INCONCLUSIVE, with the board's own partition table and
    git's partitions.csv printed side by side as the decision requires,
    never dropped."""
    from .. import dashboard_http_client, partition_http_client

    host = ctx["host"]
    board_partitions: Any = None
    try:
        board_partitions = partition_http_client.get_partitions(host)
    except Exception as exc:  # noqa: BLE001
        board_partitions = f"error: {exc}"

    csv_path = ctx.get("partitions_csv_path") or os.path.join(
        ctx.get("repo_root") or _repo_root(), "firmware", "KilnFW", "partitions.csv"
    )
    try:
        with open(csv_path, "r", encoding="utf-8") as f:
            partitions_csv = f.read()
    except OSError as exc:
        partitions_csv = f"error: {exc}"

    tables = {"board_partitions": board_partitions, "partitions_csv": partitions_csv}

    try:
        data = dashboard_http_client.get_cfgfs_status(host)
        pending_data = dashboard_http_client.get_cfgfs_format_pending(host)
    except Exception as exc:  # noqa: BLE001
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"GET /api/cfgfs or /api/cfgfs/format_pending failed: {exc} (record-only, plan §7 decision 6)",
            observed=dict(tables, cfgfs="error: " + str(exc)),
        )
    ctx["_fl07_cfgfs"] = data  # for WEB-DIAG-04
    result = J.judge_cfgfs_state(data, pending_data)
    if result.observed is not None:
        result.observed = dict(result.observed, **tables)
    return result


def _case_fl08(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    fw_text = srv.safety_get_fw_version()
    diag_text = srv.safety_get_diag()
    commit_match = re.search(r"\b([0-9a-fA-F]{6,40})\b", fw_text)
    commit = commit_match.group(1) if commit_match else None
    boot_reason = None
    m = re.search(r"boot[_ ]reason[:=]\s*([a-zA-Z_]+)", diag_text)
    if m:
        boot_reason = m.group(1).lower()
    ctx["_fl08_commit"] = commit
    return J.judge_pico_slot_metadata(commit, boot_reason)


def _case_fl09(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    commit = ctx.get("_fl08_commit")
    if not commit:
        return CaseResult(Verdict.NOT_RUN, reason="dependency FL-08 was not PASS", observed={})
    text = srv.find_safty_crash_elf(commit)
    return J.judge_archived_elf_matches(text)


# ---------------------------------------------------------------------------
# SK-01/03/04 -- stack margins and heap floor.
# ---------------------------------------------------------------------------

def _case_sk01(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    text = srv.get_stack_margin()
    return J.judge_stack_margin(text)


def _case_sk03(ctx: dict) -> CaseResult:
    host = ctx["host"]
    status, body = _http_get_json(host, "/api/saftyfw_stack_margin")
    if status != 200 or not isinstance(body, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/saftyfw_stack_margin: status={status}", observed={"body": body})
    tasks = body.get("tasks", [])
    return J.judge_pico_stack_margins(tasks)


def _case_sk04(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    from .. import dashboard_http_client

    host = ctx["host"]
    before_text = srv.get_heap_status(host=host)
    unacknowledged = "UNACKNOWLEDGED CRASH REPORT" in before_text
    try:
        heap = dashboard_http_client.get_heap_status(host)
        internal = heap.get("heap_internal", {})
        largest = internal.get("largest_free_block")
    except Exception:  # noqa: BLE001
        largest = None
    return J.judge_heap_dram_floor(largest, largest, unacknowledged)


# ---------------------------------------------------------------------------
# SP-01/02/05/07 -- safety processor read-backs.
# ---------------------------------------------------------------------------

def _case_sp01(ctx: dict) -> CaseResult:
    from .. import safety_cfg_http_client, zones_http_client

    host = ctx["host"]
    try:
        commissioning = safety_cfg_http_client.get_commissioning(host)
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.FAIL, reason=f"GET /api/safety/commissioning failed: {exc}", observed={})
    esp_max_temp_c = None
    try:
        zones = zones_http_client.get_zones(host)
        esp_max_temp_c = zones.get("max_temp_c") or zones.get("abs_max_temp_c")
    except Exception:  # noqa: BLE001
        pass
    return J.judge_commissioning_readback(commissioning, esp_max_temp_c)


def _case_sp02(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    status_text = srv.safety_get_status()
    diag_text = srv.safety_get_diag()
    link_up = "link" in status_text.lower() and "down" not in status_text.lower()
    boot_reason = None
    m = re.search(r"boot[_ ]reason[:=]\s*([a-zA-Z_]+)", diag_text)
    if m:
        boot_reason = m.group(1).lower()
    # trip_reason/trip_mask live only in safety_get_diag()'s text -- NOT
    # safety_get_status()'s (SafetyStatus carries neither field; see
    # cases_safety.py's identical fix for SP-08/SP-09). Parsing them off
    # status_text can only ever fail to match, and a silent default to 0
    # would then always pass this consistency check regardless of the
    # board's real trip state -- J.parse_trip_reason returns None on no
    # match, and judge_status_diag_consistency treats that as INCONCLUSIVE,
    # never as an assumed-healthy 0.
    trip_reason = J.parse_trip_reason(diag_text)
    trip_mask = J.parse_trip_mask(diag_text)
    state = "armed" if "armed" in status_text.lower() else "idle"
    return J.judge_status_diag_consistency(link_up, state, boot_reason, trip_reason, trip_mask=trip_mask)


def _case_sp05(ctx: dict) -> CaseResult:
    """SP-05: E-stop interlock is not asserted.

    Read-only, via the cached GET_STATUS flags (srv.safety_get_status(),
    same source as SP-02) -- NEVER POST /api/estop/verify. That route
    (diagnostics_http.c ~line 532, estop_verification.h) is an ADMIN WRITE:
    it records that an operator has physically verified the E-stop
    interlock, the same effect as the diagnostics page's own button. A
    read-only smoke case must not perform that write; whether the interlock
    currently reads asserted is fully answerable from the cached status
    text already fetched for SP-02.

    safety_get_status() itself can raise a bare TimeoutError (link_hub.py's
    hub-request path, link_hub.py:573) rather than returning an "error: ..."
    string, when the hub/link is unavailable -- caught here and treated as
    INCONCLUSIVE, same as the "error: ..." case judge_estop_verify already
    handles, rather than letting it propagate and have the runner record a
    bare FAIL with no safety-relevant information."""
    srv = _srv(ctx)
    try:
        status_text = srv.safety_get_status()
    except TimeoutError as exc:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"safety_get_status() timed out (hub/link unavailable): {exc}",
            observed={},
        )
    return J.judge_estop_verify(status_text)


def _case_sp07(ctx: dict) -> CaseResult:
    """SP-07: consistent max_rate_c_per_min read-back between the safety
    side (srv.safety_get_rate_guard(), Pico-sourced) and the ESP side
    (GET /api/safety/rate_guard/auto).

    2026-09-24 fix: this used to report "rate guard fields missing on one
    side" for two unrelated reasons collapsed into one generic message --
    (a) srv.safety_get_rate_guard() itself returning an "error: ..." string
    (link/hub unavailable) and (b) GET /api/safety/rate_guard/auto legitimately
    answering ``{"ok": false, "reason": "..."}`` whenever no zone has a usable
    identification yet (safety_cfg_http.c's rate_guard_auto_get_handler() --
    the S8 auto-calc estimate needs at least one autotuned zone; this is the
    live bench's normal state, not a parser failure). Both are now surfaced
    as their own INCONCLUSIVE reason instead of the generic one. Separately,
    the route's SUCCESS shape reports the ESP's current value under
    ``current_c_per_min`` -- not ``max_rate_c_per_min`` -- so the previous
    parser could never have matched it even on an ``ok:true`` reply."""
    srv = _srv(ctx)

    text = srv.safety_get_rate_guard()
    if text.startswith("error"):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=text,
            observed={"safety_get_rate_guard": text},
        )
    safety_side: dict = {}
    m = re.search(r"max_rate_c_per_min[:=]\s*([\d.]+)", text)
    if m:
        safety_side["max_rate_c_per_min"] = float(m.group(1))
    host = ctx["host"]
    status, body = _http_get_json(host, "/api/safety/rate_guard/auto")
    if status != 200 or not isinstance(body, dict):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"GET /api/safety/rate_guard/auto: status={status}",
            observed={"safety_side": safety_side, "status": status, "body": body},
        )
    if not body.get("ok"):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"/api/safety/rate_guard/auto: {body.get('reason', 'ok:false with no reason given')}",
            observed={"safety_side": safety_side, "esp_side": body},
        )
    esp_side = {"max_rate_c_per_min": body.get("current_c_per_min")}
    return J.judge_rate_guard_consistency(safety_side, esp_side)


#: Wire the smoke suite's judge functions into the shared REGISTRY (see
#: registry.py's module docstring for why the ids themselves are declared
#: there rather than here).
_CASE_FUNCS = {
    "ST-05": _case_st05,
    "FL-01": _case_fl01,
    "FL-02": _case_fl02,
    "FL-03": _case_fl03,
    "FL-04": _case_fl04,
    "FL-05": _case_fl05,
    "FL-06": _case_fl06,
    "FL-07": _case_fl07,
    "FL-08": _case_fl08,
    "FL-09": _case_fl09,
    "SK-01": _case_sk01,
    "SK-03": _case_sk03,
    "SK-04": _case_sk04,
    "SP-01": _case_sp01,
    "SP-02": _case_sp02,
    "SP-05": _case_sp05,
    "SP-07": _case_sp07,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
