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


# ---------------------------------------------------------------------------
# FL-10/FL-11 -- Wave 3b, opt-in JTAG flash round trips (plan §8 Wave 3
# part B). Neither runs in `nightly`/`full` by default -- both are gated by
# `ctx.get("allow_flash")`, SKIP otherwise, per the plan's "optional,
# opt-in" column. Neither is gated by --attended: owner decision 10 keeps
# operator_only (registry.py) purely descriptive here, since these two are
# allowed to run unattended once opted into -- conflating the two gates
# would contradict that decision.
# ---------------------------------------------------------------------------

_FL10_APP_PARTITION = "app"
#: SAFETY_TRIP_MAIN_FAULT = 6 (S6a) -- firmware/SaftyFW/src/safety_guards.h.
#: Expected during FL-11's dual-reflash window while the ESP's safety-link
#: handshake is still coming up (CLAUDE.md: "expected, not a bug").
_FL11_EXPECTED_TRIP_REASON = 6


def _case_fl10(ctx: dict) -> CaseResult:
    """FL-10: ESP JTAG flash round trip. Opt-in via `ctx["allow_flash"]`;
    also needs `ctx["ap_password"]` (flash_firmware()'s boot_guard_reset and
    verification path use it -- see mcp_server_flash.py). A refusal or
    exception from the flashing tool itself is always a FAIL, never a
    lesser verdict (plan §6 rule 3) -- `judgments.judge_flash_round_trip`
    treats any `error:`-prefixed reply that way."""
    if not ctx.get("allow_flash"):
        return CaseResult(Verdict.SKIP, reason="opt-in: pass allow_flash=True to run FL-10")
    ap_password = ctx.get("ap_password")
    if not ap_password:
        return CaseResult(Verdict.SKIP, reason="opt-in: FL-10 requires ap_password")

    srv = _srv(ctx)
    kwargs = {"verify": True, "ap_password": ap_password}
    kiln_fw_root = ctx.get("kiln_fw_root")
    if kiln_fw_root:
        kwargs["kiln_fw_root"] = kiln_fw_root
    try:
        text = srv.flash_firmware(**kwargs)
    except Exception as exc:  # noqa: BLE001
        return J.judge_flash_round_trip(None, None, _FL10_APP_PARTITION, error=str(exc))

    if text.startswith("error:"):
        return J.judge_flash_round_trip(None, None, _FL10_APP_PARTITION, error=text)

    running_match = re.search(r"running\s+'(\w+)'", text)
    running_partition = running_match.group(1) if running_match else None
    if "confirmed the board is running" in text:
        return J.judge_flash_round_trip(True, running_partition, _FL10_APP_PARTITION, error=None)
    if "WARNING" in text:
        return J.judge_flash_round_trip(False, running_partition, _FL10_APP_PARTITION, error=None)
    # Flashed OK but verify=False's early return, or some other shape this
    # parsing doesn't recognize yet -- record-only rather than guessing.
    return J.judge_flash_round_trip(None, running_partition, _FL10_APP_PARTITION, error=None)


def _case_fl11(ctx: dict) -> CaseResult:
    """FL-11: Pico JTAG flash round trip (opt-in via `allow_flash`), then
    the S6a procedure of plan §6: confirm link comes back up with ONLY
    SAFETY_TRIP_MAIN_FAULT latched (never a different or additional guard),
    then clear it and confirm the clear. `debug_program(peer="pico")`
    failing outright is a FAIL via the same `judge_flash_round_trip` path
    FL-10 uses; the S6a check itself reuses `judge_operator_trip` (it is
    generic trip-then-clear reasoning, not actually operator-specific)."""
    if not ctx.get("allow_flash"):
        return CaseResult(Verdict.SKIP, reason="opt-in: pass allow_flash=True to run FL-11")

    srv = _srv(ctx)
    try:
        flash_text = srv.debug_program(peer="pico")
    except Exception as exc:  # noqa: BLE001
        return J.judge_flash_round_trip(None, None, "pico", error=str(exc))
    if isinstance(flash_text, str) and flash_text.lower().startswith("error"):
        return J.judge_flash_round_trip(None, None, "pico", error=flash_text)

    # safety_get_diag(), not safety_get_status(): SafetyStatus carries no
    # trip_reason/trip_mask at all, so parsing the status text would report
    # "no trip_reason" and FAIL every run.
    status_text = srv.safety_get_diag()
    trip_reason = None
    m = re.search(r"trip_reason\s*[:=]?\s*(\d+)", status_text)
    if m:
        trip_reason = int(m.group(1))
    trip_mask = None
    mm = re.search(r"trip_mask\s*[:=]?\s*(0x[0-9a-fA-F]+|\d+)", status_text)
    if mm:
        trip_mask = int(mm.group(1), 0)
    expected_mask = J.safety_trip_mask_for_reason(_FL11_EXPECTED_TRIP_REASON)

    cleared_after: "bool | None" = None
    # Plan section 6 rule 5: exactly-matched reason AND mask before any
    # safety_clear_trip(). S6a is reason 6, so the mask must be 0x0020 and
    # nothing else -- a mask the report omits is not a match.
    if trip_reason == _FL11_EXPECTED_TRIP_REASON and trip_mask == expected_mask:
        srv.safety_clear_trip()
        after_text = srv.safety_get_diag()
        after_m = re.search(r"trip_reason[:=]\s*(\d+)", after_text)
        if after_m:
            cleared_after = int(after_m.group(1)) == 0

    return J.judge_operator_trip(trip_reason, _FL11_EXPECTED_TRIP_REASON, cleared_after, trip_mask=trip_mask)


#: Wire this wave's judge functions into the shared REGISTRY. Imported by
#: __init__.py after cases_smoke, so these assignments win over that
#: module's wave-0 placeholders for FL-09/SK-01/SK-03; SK-02 is new.
_CASE_FUNCS = {
    "FL-10": _case_fl10,
    "FL-11": _case_fl11,
    "SK-01": _case_sk01,
    "SK-02": _case_sk02,
    "SK-03": _case_sk03,
    "FL-09": _case_fl09,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
