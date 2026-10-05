"""Background build jobs: start now, poll later.

Why this exists: a full ``build_kilnfw`` (SaftyFW first, then KilnFW, plus any
wait for a build-gate slot) can exceed five minutes, and an MCP client's idle
watchdog gives up on a single in-flight tool call after 300 s. The build kept
running inside the server and finished, but the caller lost the result and had
to poll the build directory by hand (2026-10-04). A job registry turns that
into two short calls: ``build_kilnfw_start`` returns a job id immediately and
``build_job_status`` reports running/ok/failed plus the report.

The synchronous ``build_kilnfw`` is unchanged; nothing here replaces it.

Results are also written to ``<tmp>/kilnctl-builds/job-<id>.json`` when a job
finishes, so a status call still answers after the server restarts or after
the in-memory registry evicted the entry. A job that was *running* when the
server died has no file and reports as unknown -- check the build directory.
"""

from __future__ import annotations

import json
import math
import os
import re
import tempfile
import threading
import time
import traceback
import uuid
from typing import Any, Callable, Optional

#: Most recent jobs kept in memory; older finished ones are evicted (their
#: JSON file remains).
MAX_JOBS = 20

#: Longest a single status call may block waiting for completion. Kept well
#: under the 300 s client idle watchdog.
MAX_WAIT_S = 120.0

_STATUS_LINE = re.compile(r"^[\w.-]+: (OK|FAILED|TIMEOUT|ABORTED|error)\b")

#: Job files kept in the temp directory; older ones are pruned on each write.
MAX_JOB_FILES = 50

_JOB_ID = re.compile(r"^[0-9a-f]{8}$")

_lock = threading.Lock()
_jobs: "dict[str, dict[str, Any]]" = {}


def _job_dir() -> str:
    directory = os.path.join(tempfile.gettempdir(), "kilnctl-builds")
    os.makedirs(directory, exist_ok=True)
    return directory


def _job_file(job_id: str) -> str:
    if not _JOB_ID.match(job_id):
        raise ValueError(f"invalid job id {job_id!r}")
    return os.path.join(_job_dir(), f"job-{job_id}.json")


def _write_result(job_id: str, record: "dict[str, Any]") -> None:
    """Atomic write (temp file + os.replace), then prune old job files."""
    path = _job_file(job_id)
    tmp = f"{path}.{os.getpid()}.tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(record, handle)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
    _prune_job_files()


def _prune_job_files() -> None:
    directory = _job_dir()
    try:
        files = [os.path.join(directory, n) for n in os.listdir(directory)
                 if n.startswith("job-") and n.endswith(".json")]
        files.sort(key=os.path.getmtime, reverse=True)
    except OSError:
        return
    for stale in files[MAX_JOB_FILES:]:
        try:
            os.remove(stale)
        except OSError:
            pass


def classify_report(report: str) -> str:
    """``"ok"`` or ``"failed"`` from a workbench report.

    Every step header reads ``<tag>: OK|FAILED|TIMEOUT|ABORTED|error``. The job
    is ok only if at least one step says OK and none says anything else, so a
    SaftyFW OK followed by a KilnFW failure is a failure.
    """
    verdicts = []
    for line in report.splitlines():
        m = _STATUS_LINE.match(line.strip())
        if m:
            verdicts.append(m.group(1))
    return "ok" if verdicts and all(v == "OK" for v in verdicts) else "failed"


#: Keys of a live job dict that are not JSON-serialisable and so never persisted.
_NON_PERSISTED = ("done", "progress", "classify")


def start_job(tool: str, runner: "Callable[[], str]", params: "dict[str, Any]",
              artifacts: "Optional[list[str]]" = None,
              classify: "Optional[Callable[[str], str]]" = None,
              progress: "Optional[Callable[[], str]]" = None) -> str:
    """Run ``runner`` on a daemon thread; return the new job id.

    ``classify`` maps the finished report to the job state (default
    :func:`classify_report`, which understands workbench build reports).
    ``progress`` is called by :func:`job_status` while the job is still running
    and returns a short text (e.g. a log tail); it must never raise and is
    never persisted.
    """
    job_id = uuid.uuid4().hex[:8]
    job: "dict[str, Any]" = {
        "id": job_id, "tool": tool, "params": params, "state": "running",
        "started": time.time(), "finished": None, "report": None,
        "artifacts": list(artifacts or []), "done": threading.Event(),
        "progress": progress, "classify": classify or classify_report,
    }

    def work() -> None:
        try:
            report = runner()
            state = job["classify"](report)
        except BaseException:  # noqa: BLE001 - a job thread must never vanish silently
            report = f"{tool}: FAILED (job thread raised)\n{traceback.format_exc()}"
            state = "failed"
        finished_at = time.time()
        try:
            # Persist BEFORE publishing the state, so a caller that sees a
            # finished state can always also find the result file.
            try:
                record = {k: v for k, v in job.items() if k not in _NON_PERSISTED}
                record.update(report=report, state=state, finished=finished_at)
                _write_result(job_id, record)
            except Exception:  # noqa: BLE001 - persistence is best-effort
                pass
        finally:
            job["report"], job["finished"], job["state"] = report, finished_at, state
            job["done"].set()

    with _lock:
        _jobs[job_id] = job
        finished = [j for j in _jobs.values() if j["state"] != "running"]
        finished.sort(key=lambda j: j["started"])
        while len(_jobs) > MAX_JOBS and finished:
            _jobs.pop(finished.pop(0)["id"], None)
    threading.Thread(target=work, name=f"build-job-{job_id}", daemon=True).start()
    return job_id


def _artifact_lines(paths: "list[str]") -> "list[str]":
    lines = []
    for path in paths:
        try:
            st = os.stat(path)
        except OSError:
            lines.append(f"  {path}: missing")
            continue
        age = max(0.0, time.time() - st.st_mtime)
        lines.append(f"  {path}: {st.st_size} bytes, modified {age:.0f}s ago")
    return lines


def job_status(job_id: str, wait_s: float = 0.0, noun: str = "build-job",
               poll_tool: str = "build_job_status") -> str:
    """Report a job; optionally block up to ``wait_s`` (capped) for it to finish.

    ``noun``/``poll_tool`` only change the wording, so a non-build job (a bench
    run) reads as what it is.
    """
    try:
        wait_s = float(wait_s)
    except (TypeError, ValueError):
        wait_s = 0.0
    if not math.isfinite(wait_s) or wait_s < 0:
        wait_s = MAX_WAIT_S if wait_s == math.inf else 0.0
    if not _JOB_ID.match(str(job_id)):
        return f"{noun} {job_id!r}: invalid job id (expected 8 hex characters)"
    with _lock:
        job = _jobs.get(job_id)
    if job is None:
        try:
            with open(_job_file(job_id), encoding="utf-8") as handle:
                job = json.load(handle)
        except (OSError, ValueError):
            return (f"{noun} {job_id}: unknown -- not in this server's memory and no "
                    f"result file (the server may have restarted mid-run; check the "
                    f"build/run directory)")
    elif wait_s > 0 and job["state"] == "running":
        job["done"].wait(min(wait_s, MAX_WAIT_S))
    state = job["state"]
    end = job["finished"] or time.time()
    head = f"{noun} {job['id']} ({job['tool']}): {state.upper()} after {end - job['started']:.1f}s"
    parts = [head]
    if job["artifacts"]:
        parts.append("artifacts:\n" + "\n".join(_artifact_lines(job["artifacts"])))
    if state == "running":
        tail_fn = job.get("progress")
        if tail_fn is not None:
            try:
                tail = tail_fn()
            except Exception as exc:  # noqa: BLE001 - progress is best-effort
                tail = f"(progress unavailable: {type(exc).__name__}: {exc})"
            if tail:
                parts.append("progress:\n" + tail)
        parts.append(f"still running; call {poll_tool} again (wait_s up to "
                     f"{int(MAX_WAIT_S)} blocks until done)")
    else:
        parts.append("--\n" + (job["report"] or "(no report)"))
    return "\n".join(parts)


def _reset_for_tests() -> None:
    with _lock:
        _jobs.clear()
