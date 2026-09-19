"""summary.json + transcript.md writers (plan §2.3).

Schema follows stack_margin_baseline.py's convention: plain dict ->
``json.dumps(indent=2, sort_keys=True)``, no custom encoder. One directory
per run under ``logs/bench_test/<UTC timestamp>_<suite>[_<tag>]/`` -- a new
sibling of ``logs/coupling/``, gitignored (see the repo .gitignore entry
this wave adds).
"""
from __future__ import annotations

import dataclasses
import datetime
import json
import os
import re
from typing import Any, Dict, Optional


def make_run_id(suite: str, tag: Optional[str] = None) -> str:
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    safe_suite = re.sub(r"[^A-Za-z0-9_-]", "_", suite)
    run_id = f"{ts}_{safe_suite}"
    if tag:
        safe_tag = re.sub(r"[^A-Za-z0-9_-]", "_", tag)
        run_id = f"{run_id}_{safe_tag}"
    return run_id


def default_logs_root() -> str:
    """tools/PcTools/src/kilnctrl/bench_test/ -> repo root is five levels
    up, then logs/bench_test -- same relative-path convention as
    flash_provenance._repo_root()."""
    repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", ".."))
    return os.path.join(repo_root, "logs", "bench_test")


def run_dir_path(logs_root: Optional[str], run_id: str) -> str:
    root = logs_root or default_logs_root()
    return os.path.join(root, run_id)


def _redact(text: str) -> str:
    """Belt-and-suspenders: even though wave 0's read-only cases never
    handle a credential, transcript.md redacts anything shaped like one
    before it is ever written (plan §6 rule 9)."""
    text = re.sub(r'("ap_password"\s*:\s*")[^"]*(")', r"\1***\2", text)
    text = re.sub(r"(ap_password=)\S+", r"\1***", text)
    return text


def build_summary(outcome, board_before: Dict[str, Any], board_after: Dict[str, Any]) -> Dict[str, Any]:
    cases = {}
    for cid, result in outcome.results.items():
        cases[cid] = {
            "verdict": result.verdict,
            "reason": result.reason,
            "observed": result.observed,
            "expected": result.expected,
            "evidence": result.evidence,
        }
    return {
        "run_id": outcome.run_id,
        "suite": outcome.suite,
        "requested_cases": outcome.requested,
        "executed_cases": outcome.executed,
        "started": outcome.started,
        "ended": outcome.ended,
        "duration_s": outcome.ended - outcome.started,
        "preflight_ok": outcome.preflight_ok,
        "preflight_reason": outcome.preflight_reason,
        "tainted": outcome.tainted,
        "exit_code": outcome.exit_code,
        "cases": cases,
        "board_before": board_before,
        "board_after": board_after,
    }


def write_run(run_dir: str, summary: Dict[str, Any], transcript_lines: "list[str]") -> None:
    os.makedirs(run_dir, exist_ok=True)
    os.makedirs(os.path.join(run_dir, "captures"), exist_ok=True)

    summary_path = os.path.join(run_dir, "summary.json")
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True, default=str)

    transcript_path = os.path.join(run_dir, "transcript.md")
    with open(transcript_path, "w", encoding="utf-8") as f:
        f.write(f"# bench_test transcript -- {summary['run_id']}\n\n")
        f.write(_redact("\n".join(transcript_lines)))
        f.write("\n\n## Verdicts\n\n")
        for cid, case in summary["cases"].items():
            f.write(f"- **{cid}**: {case['verdict']} -- {case['reason']}\n")

    with open(os.path.join(run_dir, "board_before.json"), "w", encoding="utf-8") as f:
        json.dump(summary["board_before"], f, indent=2, sort_keys=True, default=str)
    with open(os.path.join(run_dir, "board_after.json"), "w", encoding="utf-8") as f:
        json.dump(summary["board_after"], f, indent=2, sort_keys=True, default=str)


def list_recent_runs(logs_root: Optional[str] = None, n: int = 1) -> "list[Dict[str, Any]]":
    root = logs_root or default_logs_root()
    if not os.path.isdir(root):
        return []
    run_dirs = sorted(
        (d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d))),
        reverse=True,
    )
    out = []
    for d in run_dirs[:n]:
        summary_path = os.path.join(root, d, "summary.json")
        if not os.path.isfile(summary_path):
            continue
        with open(summary_path, "r", encoding="utf-8") as f:
            out.append(json.load(f))
    return out
