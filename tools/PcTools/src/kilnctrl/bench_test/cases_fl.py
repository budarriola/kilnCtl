"""Wave 1d -- FL/SK baselines (docs/BENCH_TEST_SYSTEM_PLAN.md §8).

Adds/overrides judge functions for:

  * SK-01 -- ESP high-water marks, idle: now compares the live reading
    against the committed baseline (``stack_margin_baseline.load_records()``)
    instead of only checking for an outright error (wave 0's placeholder).
  * SK-02 -- ESP high-water marks, exercised: new. Same comparison, plus
    the plan's absolute floor (every task >= 512 B free, the httpd stack
    blob class -- project memory ``project_httpd_stack_blob_class``).
  * FL-09 -- Pico image vs archive: overrides wave 0's plain
    ``judge_archived_elf_matches`` to downgrade an otherwise-PASS "found"
    result to INCONCLUSIVE when the archived ELF has no sibling
    ``project_description.json`` -- ``read_esp_coredump`` needs that file
    to symbolize, and ``elf_archive/`` deliberately never carries it
    (2026-09-19 revert, see ``elf_archive.py``), so this is expected, not a
    FAIL, per the plan's "FL-09 record-only INCONCLUSIVE never FAIL" rule.
  * A §7.5 Pico baseline capture, opportunistically taken alongside SK-03's
    existing fetch (same route, no extra board round trip) and written into
    the run's own directory (``ctx["run_dir"]``, wired up in runner.py) as a
    ``PicoStackMarginBaselineRecord`` -- see stack_margin_baseline.py's new
    Pico record type.

Imported by ``__init__.py`` *after* ``cases_smoke`` so this module's
``get_case(id).judge = ...`` reassignments win (see that file's own import
order comment). This is the "ONE appended line" sibling waves rebase past.
"""
from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Optional

from . import judgments as J
from .cases_smoke import _http_get_json, _repo_root, _srv
from .registry import CaseResult, Verdict, get_case

#: Baseline records for the ESP side are committed here -- a sibling of the
#: DRAM_PSRAM_PLAN.md measurement procedure's own convention
#: (tools/PcTools/scripts/capture_stack_margin_baseline.py's default), so a
#: bench-test run compares against the same directory a human capture would
#: read/write by hand.
_ESP_BASELINE_DIR = "docs/stack_margin_baseline"

#: Where each run's own Pico baseline capture is written (plan §7.5): the
#: run's own directory, not the committed ESP baseline dir above -- the Pico
#: baseline is accumulated/reviewed separately (wave 1 still building up a
#: real threshold; see the flat-25% interim rule note in judgments.py), not
#: compared against on every run the way the ESP one is.
_PICO_BASELINE_SUBDIR = "pico_stack_margin"

#: SK-02's absolute floor, independent of any committed baseline (plan §3.3):
#: every task must have at least this many bytes free even before comparing
#: against history -- the httpd stack blob class (project memory
#: project_httpd_stack_blob_class) is exactly a task that looked fine on a
#: relative/percentage basis but was dangerously close in absolute terms.
_SK02_MIN_FREE_BYTES = 512


def _baseline_dir(ctx: dict) -> Path:
    repo_root = ctx.get("repo_root") or _repo_root()
    return Path(repo_root) / _ESP_BASELINE_DIR


def _esp_entries_and_fw(ctx: dict):
    """Live structured fetch: (entries, fw_version) or raises."""
    srv = _srv(ctx)
    entries = srv._info.get_stack_margin()
    fw_version = srv._info.get_fw_version()
    return entries, fw_version


def _write_esp_capture(ctx: dict, condition: str, entries, fw_version, notes: str = ""):
    """Best-effort: writes a fresh capture into this run's own directory (not
    the committed baseline dir -- a run never edits the committed baseline
    itself) so a run always leaves a record of what it saw, win or lose.
    Never raises -- a write failure here must not turn a real verdict into a
    crash."""
    from .. import stack_margin_baseline as smb

    run_dir = ctx.get("run_dir")
    if not run_dir:
        return None
    try:
        record = smb.build_record(condition, entries, fw_version, notes=notes)
        return smb.write_record(record, Path(run_dir) / "stack_margin_baseline")
    except Exception:  # noqa: BLE001 -- best-effort only
        return None


def _judge_against_baseline(ctx: dict, condition: str, min_free_bytes: Optional[int]) -> CaseResult:
    from .. import stack_margin_baseline as smb

    try:
        entries, fw_version = _esp_entries_and_fw(ctx)
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.FAIL, reason=f"get_stack_margin/get_fw_version failed: {exc}", observed={})

    _write_esp_capture(ctx, condition, entries, fw_version, notes=f"bench_test {condition} capture")

    committed = smb.load_records(_baseline_dir(ctx))
    baseline_by_name = smb.worst_case_across_conditions(committed)

    return J.judge_stack_margin_against_baseline(entries, baseline_by_name, min_free_bytes=min_free_bytes)


def _case_sk01(ctx: dict) -> CaseResult:
    return _judge_against_baseline(ctx, condition="idle", min_free_bytes=None)


def _case_sk02(ctx: dict) -> CaseResult:
    return _judge_against_baseline(ctx, condition="web_ui_open", min_free_bytes=_SK02_MIN_FREE_BYTES)


# ---------------------------------------------------------------------------
# FL-09 -- override: downgrade "found" to INCONCLUSIVE when there is no
# project_description.json beside the archived ELF.
# ---------------------------------------------------------------------------

def _case_fl09(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    commit = ctx.get("_fl08_commit")
    if not commit:
        return CaseResult(Verdict.NOT_RUN, reason="dependency FL-08 was not PASS", observed={})
    text = srv.find_safty_crash_elf(commit)
    return J.judge_pico_archive_with_description(text, description_exists=_description_json_exists)


def _description_json_exists(elf_path: str) -> bool:
    return os.path.isfile(os.path.join(os.path.dirname(elf_path), "project_description.json"))


# ---------------------------------------------------------------------------
# §7.5 Pico baseline capture -- opportunistic, alongside SK-03's own fetch.
# ---------------------------------------------------------------------------

def _pico_build_identity(ctx: dict) -> "tuple[str, str, str]":
    """(commit, build_date, build_time) from safety_get_fw_version()'s text
    report -- the same text FL-08 already parses for its commit, so this
    reuses that regex rather than inventing a second one. Build date/time
    are best-effort: SaftyFW's fw_version text may not carry them in every
    firmware revision, so a missing match degrades to empty strings rather
    than raising."""
    srv = _srv(ctx)
    fw_text = srv.safety_get_fw_version()
    commit = ctx.get("_fl08_commit")
    if not commit:
        m = re.search(r"\b([0-9a-fA-F]{6,40})\b", fw_text)
        commit = m.group(1) if m else "unknown"
    date_m = re.search(r"\b(\d{4}-\d{2}-\d{2})\b", fw_text)
    time_m = re.search(r"\b(\d{2}:\d{2}:\d{2})\b", fw_text)
    return commit, (date_m.group(1) if date_m else ""), (time_m.group(1) if time_m else "")


def _capture_pico_baseline(ctx: dict, tasks: "list[dict]", body: dict) -> None:
    """Best-effort, never raises, never affects SK-03's own verdict --
    writes into this run's own directory (plan §7.5: 'capture a Pico
    baseline in wave 1'), not a committed location, since wave 1 is still
    accumulating data toward a real threshold (owner decision 4)."""
    from .. import stack_margin_baseline as smb

    run_dir = ctx.get("run_dir")
    if not run_dir:
        return
    try:
        commit, build_date, build_time = _pico_build_identity(ctx)
        entries = tuple(
            smb.PicoStackMarginEntry(
                task_id=int(t.get("id", 0)),
                name=t.get("name", "?"),
                measured=bool(t.get("measured", False)),
                high_water_words=t.get("high_water_words"),
                stack_total_words=t.get("stack_total_words"),
            )
            for t in tasks
        )
        record = smb.build_pico_record(
            entries,
            saftyfw_commit=commit,
            saftyfw_build_date=build_date,
            saftyfw_build_time=build_time,
            rounds_completed=int(body.get("rounds_completed", 0)),
            last_tick_ms=int(body.get("last_tick_ms", 0)),
            all_measured=bool(body.get("all_measured", False)),
            notes="bench_test SK-03 opportunistic capture (plan section 7.5)",
        )
        smb.write_pico_record(record, Path(run_dir) / _PICO_BASELINE_SUBDIR)
    except Exception:  # noqa: BLE001 -- best-effort only, never sinks SK-03
        pass


def _case_sk03(ctx: dict) -> CaseResult:
    """Same fetch/verdict as wave 0's SK-03, with the §7.5 baseline capture
    added as a side effect on a successful read -- no extra board round
    trip, and a capture failure never changes SK-03's own verdict."""
    host = ctx["host"]
    status, body = _http_get_json(host, "/api/saftyfw_stack_margin")
    if status != 200 or not isinstance(body, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/saftyfw_stack_margin: status={status}", observed={"body": body})
    tasks = body.get("tasks", [])
    _capture_pico_baseline(ctx, tasks, body)
    return J.judge_pico_stack_margins(tasks)


#: Wire this wave's judge functions into the shared REGISTRY. Imported by
#: __init__.py after cases_smoke, so these assignments win over that
#: module's wave-0 placeholders for FL-09/SK-01/SK-03; SK-02 is new.
_CASE_FUNCS = {
    "SK-01": _case_sk01,
    "SK-02": _case_sk02,
    "SK-03": _case_sk03,
    "FL-09": _case_fl09,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
