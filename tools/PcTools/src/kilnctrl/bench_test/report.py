"""summary.json + transcript.md writers (plan §2.3).

Schema follows stack_margin_baseline.py's convention: plain dict ->
``json.dumps(indent=2, sort_keys=True)``, no custom encoder. One directory
per run under ``logs/bench_test/<UTC timestamp>_<suite>[_<tag>]/`` -- a new
sibling of ``logs/coupling/``, gitignored (see the repo .gitignore entry
this wave adds).
"""
from __future__ import annotations

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


_CREDENTIAL_KEY_RE = re.compile(
    r'("(?:ap_password|password|wifi_password|web_password|psk|ssid_password)"\s*:\s*")[^"]*(")',
    re.IGNORECASE,
)
_CREDENTIAL_QUERY_RE = re.compile(r"((?:ap_password|password|psk)=)\S+", re.IGNORECASE)

#: Env var names whose *values* (KILNCTL_WEB_PASSWORD, and any Wi-Fi
#: credential a wave 1+ commissioning case sources per plan §7 decision 7)
#: must never appear literally in a written artifact, not just their key.
_CREDENTIAL_ENV_NAME_RE = re.compile(r"PASSWORD|PSK", re.IGNORECASE)


def _redact(text: str) -> str:
    """Belt-and-suspenders: even though wave 0's read-only cases never
    handle a credential, every artifact this module writes (transcript.md,
    summary.json, board_before.json, board_after.json) is redacted before
    it is ever written (plan §6 rule 9) -- both by key shape and by
    scanning for the literal value of any credential-shaped environment
    variable (e.g. ``KILNCTL_WEB_PASSWORD``, a Wi-Fi password) that
    happens to be set, in case it leaked into observed/reason text."""
    text = _CREDENTIAL_KEY_RE.sub(r"\1***\2", text)
    text = _CREDENTIAL_QUERY_RE.sub(r"\1***", text)
    for name, value in os.environ.items():
        if value and _CREDENTIAL_ENV_NAME_RE.search(name):
            text = text.replace(value, "***")
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


def write_run(run_dir: str, summary: Dict[str, Any], transcript_lines: "list[str]",
              log_doc_path: Optional[str] = None) -> None:
    os.makedirs(run_dir, exist_ok=True)
    os.makedirs(os.path.join(run_dir, "captures"), exist_ok=True)

    summary_path = os.path.join(run_dir, "summary.json")
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write(_redact(json.dumps(summary, indent=2, sort_keys=True, default=str)))

    transcript_path = os.path.join(run_dir, "transcript.md")
    with open(transcript_path, "w", encoding="utf-8") as f:
        f.write(f"# bench_test transcript -- {summary['run_id']}\n\n")
        f.write(_redact("\n".join(transcript_lines)))
        footer_lines = ["", "", "## Verdicts", ""]
        if summary.get("tainted"):
            footer_lines.insert(2, "**TAINTED**: a case could not restore board state; check the board before trusting it.")
            footer_lines.insert(3, "")
        for cid, case in summary["cases"].items():
            footer_lines.append(f"- **{cid}**: {case['verdict']} -- {case['reason']}")
        f.write(_redact("\n".join(footer_lines)) + "\n")

    with open(os.path.join(run_dir, "board_before.json"), "w", encoding="utf-8") as f:
        f.write(_redact(json.dumps(summary["board_before"], indent=2, sort_keys=True, default=str)))
    with open(os.path.join(run_dir, "board_after.json"), "w", encoding="utf-8") as f:
        f.write(_redact(json.dumps(summary["board_after"], indent=2, sort_keys=True, default=str)))

    # One human-readable line per run in docs/BENCH_TEST_LOG.md (plan §7
    # decision 1) -- best-effort: a doc-write failure (e.g. read-only
    # worktree) must never take down an otherwise-successful run.
    try:
        append_log_line(summary, run_dir, doc_path=log_doc_path)
    except OSError:
        pass


def _repo_root() -> str:
    """Same relative-path convention as default_logs_root()."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", ".."))


def default_log_doc_path() -> str:
    return os.path.join(_repo_root(), "docs", "BENCH_TEST_LOG.md")


_LOG_HEADER = (
    "# Bench Test Log\n\n"
    "One line per `bench_test_run()` call, newest last, appended automatically\n"
    "by `report.append_log_line()` (plan §7 owner decision 1: gitignored\n"
    "`logs/bench_test/<run>/` for the full machine record, plus a human sentence\n"
    "here). Never edit a past line by hand -- append only. A line never carries a\n"
    "credential; anything that looks like one is `***` before it is written, same\n"
    "as `transcript.md`/`summary.json` (`_redact()`).\n\n"
)


def _verdict_counts(summary: Dict[str, Any]) -> Dict[str, int]:
    counts = {v: 0 for v in ("PASS", "FAIL", "SKIP", "INCONCLUSIVE", "NOT_RUN")}
    for case in summary.get("cases", {}).values():
        verdict = case.get("verdict")
        if verdict in counts:
            counts[verdict] += 1
    return counts


def _fw_build_summary(summary: Dict[str, Any]) -> "tuple[str, str]":
    """Best-effort one-word-ish fw identity for each processor, read back
    from whatever `board_before` already collected during preflight
    (`runner.py`'s `esp_fw_build` / `safety_get_fw_version` entries) --
    never a fresh board round trip of its own, so a harness-error run (no
    board contact at all) still gets a log line, just with `unknown`."""
    board_before = summary.get("board_before") or {}
    esp = board_before.get("esp_fw_build") or "unknown"
    pico_raw = board_before.get("safety_get_fw_version") or "unknown"
    # `SafetyFwVersion.describe()` is a multi-field human sentence
    # ("commit=... build=... dirty=..."); keep only the first token-ish
    # chunk so the log line stays one line and skimmable.
    pico = str(pico_raw).split(",")[0].split("\n")[0].strip() or "unknown"
    return str(esp), pico


def append_log_line(summary: Dict[str, Any], run_dir: str, doc_path: Optional[str] = None) -> str:
    """Append one redacted line to docs/BENCH_TEST_LOG.md for this run
    (plan §7 decision 1). Idempotent in *format* -- every call appends
    a freshly-formatted line built only from `summary`/`run_dir`, never
    read-modify-write of a previous line -- so two processes appending
    concurrently can only interleave whole lines, never corrupt one.
    Creates the file with its header on first use. Returns the line written
    (without the trailing newline), for tests to assert against without
    re-reading the file.
    """
    path = doc_path or default_log_doc_path()
    counts = _verdict_counts(summary)
    esp_fw, pico_fw = _fw_build_summary(summary)
    try:
        rel_run_dir = os.path.relpath(run_dir, _repo_root()).replace(os.sep, "/")
    except ValueError:
        rel_run_dir = run_dir.replace(os.sep, "/")
    line = (
        f"- `{summary.get('run_id')}` suite=`{summary.get('suite')}` "
        f"exit_code={summary.get('exit_code')} "
        f"PASS={counts['PASS']} FAIL={counts['FAIL']} "
        f"INCONCLUSIVE={counts['INCONCLUSIVE']} NOT_RUN={counts['NOT_RUN']} "
        f"SKIP={counts['SKIP']} esp_fw={esp_fw} pico_fw={pico_fw} "
        f"log=`{rel_run_dir}/`"
    )
    line = _redact(line)
    is_new = not os.path.isfile(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        if is_new:
            f.write(_LOG_HEADER)
        f.write(line + "\n")
    return line


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
