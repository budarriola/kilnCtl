"""Background build jobs (mcpkit.build_jobs + workbench.build_kilnfw_start).

Fakes only: no real build, no board. Motivation: a full build_kilnfw outlasts
the MCP client's 300 s idle watchdog and the result was lost (2026-10-04).
"""

from __future__ import annotations

import threading

import pytest

from mcpkit import build_jobs, workbench


@pytest.fixture(autouse=True)
def _isolate(tmp_path, monkeypatch):
    monkeypatch.setattr(build_jobs, "_job_dir", lambda: str(tmp_path))
    build_jobs._reset_for_tests()
    yield
    build_jobs._reset_for_tests()


def test_classify_report():
    ok = "saftyfw: OK in 3.0s (5 log lines)\nfull log: x\n\nkilnfw-build: OK in 9s (1 log lines)"
    assert build_jobs.classify_report(ok) == "ok"
    assert build_jobs.classify_report(ok + "\nsdkconfig-refresh: OK -- x") == "ok"
    assert build_jobs.classify_report("saftyfw: OK in 1s\n\nkilnfw-build: FAILED (exit 1) in 2s") == "failed"
    assert build_jobs.classify_report("kilnfw-build: ABORTED -- SaftyFW build") == "failed"
    assert build_jobs.classify_report("kilnfw: error: bad root") == "failed"
    assert build_jobs.classify_report("kilnfw-build: TIMEOUT in 1800s") == "failed"
    assert build_jobs.classify_report("") == "failed"


def test_start_returns_immediately_and_status_reports_running_then_ok():
    release = threading.Event()

    def runner():
        release.wait(5)
        return "kilnfw-build: OK in 1.0s (2 log lines)\nfull log: z"

    job_id = build_jobs.start_job("kilnfw-build", runner, {})
    running = build_jobs.job_status(job_id)
    assert "RUNNING" in running
    release.set()
    done = build_jobs.job_status(job_id, wait_s=5)
    assert "OK after" in done and "kilnfw-build: OK in 1.0s" in done


def test_runner_exception_becomes_failed_not_lost():
    def runner():
        raise RuntimeError("boom")

    job_id = build_jobs.start_job("kilnfw-build", runner, {})
    out = build_jobs.job_status(job_id, wait_s=5)
    assert "FAILED" in out and "boom" in out


def test_result_survives_registry_loss_via_file():
    job_id = build_jobs.start_job("kilnfw-build", lambda: "kilnfw-build: OK in 1s", {})
    build_jobs.job_status(job_id, wait_s=5)
    build_jobs._reset_for_tests()  # simulates a restart / eviction
    assert "OK after" in build_jobs.job_status(job_id)


def test_unknown_job():
    assert "unknown" in build_jobs.job_status("deadbeef")


def test_artifact_sizes_reported(tmp_path):
    art = tmp_path / "KilnCtrl.bin"
    art.write_bytes(b"x" * 123)
    job_id = build_jobs.start_job(
        "kilnfw-build", lambda: "kilnfw-build: OK in 1s", {},
        artifacts=[str(art), str(tmp_path / "nope.elf")])
    out = build_jobs.job_status(job_id, wait_s=5)
    assert "123 bytes" in out and "nope.elf: missing" in out


def test_build_kilnfw_start_wraps_build_kilnfw(monkeypatch):
    calls = []

    def fake_build(**kwargs):
        calls.append(kwargs)
        return "saftyfw: OK in 1s\n\nkilnfw-build: OK in 2s"

    monkeypatch.setattr(workbench, "build_kilnfw", fake_build)
    started = workbench.build_kilnfw_start(target="build", jobs=4, skip_saftyfw=False)
    job_id = started.split()[1].rstrip(":")
    assert "STARTED" in started
    out = workbench.build_job_status(job_id, wait_s=5)
    assert "OK after" in out
    assert calls == [{"target": "build", "jobs": 4, "skip_saftyfw": False, "kiln_fw_root": None}]


def test_tools_registered_in_dut_bundle():
    assert "build_kilnfw_start" in workbench.BUNDLES["dut"]
    assert "build_job_status" in workbench.BUNDLES["dut"]
