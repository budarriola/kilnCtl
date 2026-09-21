"""Wave 0's only implemented cases: the read-only `smoke` suite.

Each `_case_XXX(ctx)` function does the fetching (calling the Python
functions the sibling kilnctrl MCP tools wrap -- never a second MCP server,
never hardware directly, per the plan doc §2.2) and hands the result to a
pure judge function from judgments.py. `ctx` is the dict the runner builds
per-run: `host` (board LAN/AP address), `ap_password` (optional, redacted
before it is ever logged), and a `srv` module reference (kilnctrl.mcp_server,
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
import urllib.error
import urllib.request
from typing import Any, Optional

from . import judgments as J
from .registry import CaseResult, Verdict, get_case


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
    /api/estop/verify, an admin write."""
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.getcode()
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8", errors="replace") if exc.fp else ""
    except (urllib.error.URLError, OSError) as exc:
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
    except Exception as exc:  # noqa: BLE001
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"GET /api/cfgfs failed: {exc} (record-only, plan §7 decision 6)",
            observed=dict(tables, cfgfs="error: " + str(exc)),
        )
    result = J.judge_cfgfs_state(data)
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
    text already fetched for SP-02."""
    srv = _srv(ctx)
    status_text = srv.safety_get_status()
    return J.judge_estop_verify(status_text)


def _case_sp07(ctx: dict) -> CaseResult:
    srv = _srv(ctx)

    text = srv.safety_get_rate_guard()
    safety_side: dict = {}
    m = re.search(r"max_rate_c_per_min[:=]\s*([\d.]+)", text)
    if m:
        safety_side["max_rate_c_per_min"] = float(m.group(1))
    host = ctx["host"]
    status, body = _http_get_json(host, "/api/safety/rate_guard/auto")
    esp_side = body if status == 200 and isinstance(body, dict) else {}
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
