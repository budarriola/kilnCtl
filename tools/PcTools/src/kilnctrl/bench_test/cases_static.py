"""ST-01..04 -- the static/host layer (docs/BENCH_TEST_SYSTEM_PLAN.md 3.1).

Each case wraps one ``mcpkit.workbench`` function. Those return a text
report whose first line is ``<tag>: OK|FAILED (exit N)|TIMEOUT in ...``; the
judges parse that line. The workbench functions launch REAL builds, so
tests inject fakes via ctx (``run_repo_checks_fn``, ``run_pctools_tests_fn``,
``build_saftyfw_host_tests_fn``, ``build_kilnfw_fn``); nothing here runs one
at import time.

There is no ERROR verdict in registry.Verdict: output that cannot be parsed
is reported INCONCLUSIVE with a reason starting ``ERROR:`` -- never PASS.

ST-01 baseline: ``ctx["st01_baseline_passed"]`` (int), else the newest
``summary.json`` under ``ctx["logs_root"]`` whose ST-01 observed a pass
count. With no baseline the counts are only recorded.
"""
from __future__ import annotations

import glob
import json
import os
import re
from typing import Optional

from .registry import CaseResult, Verdict, get_case

#: SaftyFW's MSVC host-test build overflows the command line on long roots
#: (CLAUDE.md: needs a short worktree path such as C:\wt\...).
SHORT_PATH_LIMIT = 40

_STATUS_RE = re.compile(r"^[^\n]*?:\s*(OK|TIMEOUT|FAILED \(exit (-?\d+)[^)]*\))", re.M)
_COUNTS_RE = re.compile(
    r"(\d+) passed, (\d+) skipped(?: \((\d+) due to -Fast\))?(?:, (\d+) BUSY \(not run\))?, (\d+) failed")


def _exit_status(text: str) -> "tuple[Optional[int], str]":
    """(exit code or None, status word)."""
    # Parse EVERY status line (build_kilnfw prints one per tag, e.g.
    # "saftyfw: OK" then "kilnfw: FAILED"); the worst one wins. FAILED is a
    # failure whatever its exit code ("FAILED (exit 0, but output check failed)").
    ms = list(_STATUS_RE.finditer(text or ""))
    if not ms:
        return None, "unparseable"
    failed = [m for m in ms if m.group(1).startswith("FAILED")]
    if failed:
        rc = next((int(m.group(2)) for m in failed if int(m.group(2)) != 0), None)
        return (rc if rc is not None else -1), "FAILED"
    if any(m.group(1) == "TIMEOUT" for m in ms):
        return None, "TIMEOUT"
    return 0, "OK"


def _unparseable(tag: str, text: str) -> CaseResult:
    return CaseResult(Verdict.INCONCLUSIVE,
                      reason=f"ERROR: could not parse {tag} result status line",
                      observed={"head": (text or "")[:300]})


def _exit_only(tag: str, text: str) -> "tuple[Optional[CaseResult], dict]":
    rc, word = _exit_status(text)
    obs = {"exit": rc, "status": word}
    if word == "unparseable":
        return _unparseable(tag, text), obs
    if word == "TIMEOUT":
        return CaseResult(Verdict.FAIL, reason=f"{tag} timed out", observed=obs), obs
    if word == "FAILED" or rc != 0:
        return CaseResult(Verdict.FAIL, reason=f"{tag} exit {rc} (status {word})", observed=obs), obs
    return None, obs


def parse_check_counts(text: str) -> "Optional[dict]":
    m = None
    for m in _COUNTS_RE.finditer(text or ""):
        pass
    if m is None:
        return None
    return {"passed": int(m.group(1)), "skipped": int(m.group(2)), "failed": int(m.group(5))}


def _baseline_from_logs(logs_root: Optional[str]) -> Optional[int]:
    if not logs_root:
        return None
    paths = glob.glob(os.path.join(logs_root, "*", "summary.json"))
    paths.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    for p in paths:
        try:
            with open(p, "r", encoding="utf-8") as fh:
                obs = ((json.load(fh).get("cases") or {}).get("ST-01") or {}).get("observed") or {}
            if isinstance(obs.get("passed"), int):
                return obs["passed"]
        except (OSError, ValueError):
            continue
    return None


def judge_repo_checks(text: str, baseline: Optional[int]) -> CaseResult:
    early, obs = _exit_only("repo-checks", text)
    if early is not None:
        return early
    counts = parse_check_counts(text)
    if counts is None:
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason="ERROR: exit 0 but no 'N passed, M skipped, K failed' line found",
                          observed=obs)
    obs.update(counts)
    obs["baseline_passed"] = baseline
    if baseline is not None and counts["passed"] < baseline:
        return CaseResult(Verdict.FAIL,
                          reason=f"pass count dropped: {counts['passed']} < baseline {baseline}",
                          observed=obs, expected={"passed_at_least": baseline})
    return CaseResult(Verdict.PASS, observed=obs)


def _fn(ctx: dict, key: str, name: str):
    f = ctx.get(key)
    if f is not None:
        return f
    from mcpkit import workbench  # local import: keeps module import free of build tooling
    return getattr(workbench, name)


def _case_st01(ctx: dict) -> CaseResult:
    text = _fn(ctx, "run_repo_checks_fn", "run_repo_checks")()
    baseline = ctx.get("st01_baseline_passed")
    if baseline is None:
        baseline = _baseline_from_logs(ctx.get("logs_root"))
    return judge_repo_checks(text, baseline)


def _case_st02(ctx: dict) -> CaseResult:
    text = _fn(ctx, "run_pctools_tests_fn", "run_pctools_tests")()
    early, obs = _exit_only("pctools-tests", text)
    return early or CaseResult(Verdict.PASS, observed=obs)


def _case_st03(ctx: dict) -> CaseResult:
    root = ctx.get("repo_root") or os.getcwd()
    limit = ctx.get("short_path_limit", SHORT_PATH_LIMIT)
    if len(root) > limit:
        return CaseResult(Verdict.SKIP,
                          reason=f"path_too_long: repo root {len(root)} chars > {limit} ({root})",
                          observed={"repo_root": root})
    text = _fn(ctx, "build_saftyfw_host_tests_fn", "build_saftyfw_host_tests")()
    early, obs = _exit_only("saftyfw-host-tests", text)
    return early or CaseResult(Verdict.PASS, observed=obs)


def _app_partition_size(kiln_fw_root: str) -> int:
    from .. import partition_table
    for e in partition_table.parse_partitions_csv(os.path.join(kiln_fw_root, "partitions.csv")):
        if e.name == "app":
            return e.size
    raise ValueError("no partition named 'app' in partitions.csv")


def _case_st04(ctx: dict) -> CaseResult:
    root = ctx.get("repo_root") or os.getcwd()
    fw_root = ctx.get("kiln_fw_root") or os.path.join(root, "firmware", "KilnFW")
    text = _fn(ctx, "build_kilnfw_fn", "build_kilnfw")()
    early, obs = _exit_only("kilnfw", text)
    if early is not None:
        return early
    bin_path = os.path.join(fw_root, "build", "KilnCtrl.bin")
    try:
        size = os.path.getsize(bin_path)
        limit = _app_partition_size(fw_root)
    except (OSError, ValueError) as exc:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"ERROR: cannot size-check image: {exc}", observed=obs)
    obs.update({"bin_bytes": size, "app_partition_bytes": limit})
    if size >= limit:
        return CaseResult(Verdict.FAIL, reason=f"KilnCtrl.bin {size} B >= app partition {limit} B",
                          observed=obs, expected={"bin_bytes_less_than": limit})
    return CaseResult(Verdict.PASS, observed=obs)


_CASE_FUNCS = {
    "ST-01": _case_st01,
    "ST-02": _case_st02,
    "ST-03": _case_st03,
    "ST-04": _case_st04,
}

for _cid, _case_fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _case_fn
