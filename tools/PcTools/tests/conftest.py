#!/usr/bin/env python3
"""Shared pytest fixtures for the PcTools suite.

Everything here is opt-in: the LIVE-BENCH fixtures skip unless
``KILNCTRL_BENCH_HOST`` names a reachable board, so the default
``pytest tools/PcTools/tests`` run (the one run_all_checks.ps1 and CI make)
stays a pure host-side run with no hardware and no mocking pretending to be
hardware.

    # host-only, the default
    python -m pytest tools/PcTools/tests -q

    # including the live-bench tests
    KILNCTRL_BENCH_HOST=192.168.1.156 python -m pytest tools/PcTools/tests -q

``bench_session`` is the reusable precondition this repo's live tests were
missing: "the board is sitting on config_presets/bench_fixture.json, and
that has been read back and confirmed". Any future live test (zone PID
behavior, autotune, dashboard reporting) takes it as an argument instead of
re-deriving what the board was configured with.
"""
from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from bench_fixture_session import (  # noqa: E402
    BENCH_HOST_ENV,
    BenchSession,
    BenchSessionError,
    bench_host,
)


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "live_bench: needs the real bench board (set KILNCTRL_BENCH_HOST)")
    config.addinivalue_line(
        "markers", "slow: minutes-long simulation test; skipped unless KILNCTL_SLOW_TESTS=1")


SLOW_TESTS_ENV = "KILNCTL_SLOW_TESTS"
SLOW_SKIP_REASON = (
    f"slow test: set {SLOW_TESTS_ENV}=1 to run it (run_pctools_tests and "
    "tools/regression_suite.py set it; the default developer run does not)")


def pytest_collection_modifyitems(config, items):
    """Skip ``@pytest.mark.slow`` tests unless KILNCTL_SLOW_TESTS=1.

    The standing runners (run_pctools_tests, regression_suite.py) export the
    variable and fail the run if this reason string shows up in the output
    (mcpkit.pytest_verdict), so the gate cannot go vacuous.
    """
    if os.environ.get(SLOW_TESTS_ENV) == "1":
        return
    if "slow" in (config.getoption("-m", default="") or ""):
        return  # an explicit `-m slow` selection means "run them"
    skip = pytest.mark.skip(reason=SLOW_SKIP_REASON)
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def _private_build_gate_dir(tmp_path_factory):
    return tmp_path_factory.mktemp("buildgate")


@pytest.fixture(autouse=True)
def _no_machine_wide_build_gate(monkeypatch, _private_build_gate_dir):
    """Keep unit tests off the machine-wide build gate.

    ``workbench.build_kilnfw``/``build_saftyfw`` wrap their (monkeypatched)
    toolchain call in ``mcpkit.buildgate.kiln_build_gate``, a Windows named-
    mutex pool shared by EVERY session on the machine, waited on for up to
    3600 s. A test that stubs only ``_run_locked`` therefore still blocks for
    as long as other sessions hold every slot -- a detached full-suite run
    sat at 28% in ``test_build_kilnfw_saftyfw_order`` with no output until
    something reaped it.

    ``KILNCTL_BUILD_GATE_SLOTS=0`` cannot be used for this: the heavy lane
    refuses a count below 1 (``_configured_slot_count``, no disabling the gate
    from a worktree). Instead every test gets a private gate: its own
    ``KILNCTL_BUILD_GATE_DIR`` (config.json, records, tickets) and its own
    per-process session-local mutex names, so it never waits on, or shows up in,
    another session's real build slots. Tests that exercise the gate itself
    (test_buildgate.py) set their own env after this runs.
    """
    tag = f"pytest_{os.getpid()}"
    monkeypatch.setenv("KILNCTL_BUILD_GATE_DIR", str(_private_build_gate_dir))
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MUTEX_PREFIX", f"Local\\kilnctl_{tag}_heavy_")
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_MUTEX_PREFIX", f"Local\\kilnctl_{tag}_light_")
    monkeypatch.delenv("KILNCTL_BUILD_GATE_SLOTS", raising=False)
    monkeypatch.delenv("KILNCTL_LIGHT_GATE_SLOTS", raising=False)
    monkeypatch.delenv("KILNCTL_BUILD_GATE_HELD", raising=False)


@pytest.fixture(autouse=True)
def _no_stale_banner_leakage(monkeypatch):
    """Isolate tests from mcp_server.py's inline per-tool-call staleness
    banner (docs/audits/mcp_staleness_banner_2026-09-14.md).

    That banner is a deliberate GLOBAL side effect, keyed off this
    process's own real, mutable source tree (mcpkit.registry.SourceSnapshot/
    check_staleness()). This suite writes real files under that tree as an
    ordinary part of running -- config presets, session logs, generated
    fixtures -- so without this fixture, an unrelated test asserting an
    exact tool return string can start failing purely because SOME OTHER
    test, earlier in the same run, happened to touch a file first (this was
    observed directly: a full-suite run produced 4 unrelated failures from
    exactly this coupling). Defaulting ``registry.freshness`` to ``None``
    here makes ``_stale_banner()`` a no-op for every test unless a test
    deliberately overrides ``mcp_server.registry`` itself --
    ``test_mcp_server_stale_banner.py`` does exactly that inside its own
    test bodies, which runs after this fixture's setup and so takes
    precedence for those tests only.
    """
    try:
        from kilnctrl import mcp_server
    except Exception:
        return
    # Patch the registry's `freshness` ATTRIBUTE only -- other tests use
    # `mcp_server.registry` itself (`.search()`, `.by_name`, ...) and must
    # keep seeing the real, fully-populated object.
    monkeypatch.setattr(mcp_server.registry, "freshness", None, raising=False)


@pytest.fixture(scope="session")
def bench_host_addr() -> str:
    host = bench_host()
    if not host:
        pytest.skip(f"no live bench: set {BENCH_HOST_ENV} to the board's address")
    return host


@pytest.fixture(scope="session")
def bench_session(bench_host_addr) -> BenchSession:
    """Session-scoped: the known-good config is written and verified ONCE,
    then shared. Writing it per-test would mean a POST /api/zones and a
    safety COMMIT_CONFIG before every assertion -- needless NVS churn on a
    board whose config nothing else in the run changes."""
    session = BenchSession(host=bench_host_addr)
    try:
        session.apply_known_good()
    except BenchSessionError as exc:
        pytest.fail(f"could not put the bench into its known-good state: {exc}")
    return session


@pytest.fixture()
def bench(bench_session) -> BenchSession:
    """Function-scoped view of the same session, with a hard stop-everything
    teardown. Use THIS in any test that can start a firing: whatever the
    test does or fails to do, the executor is stopped and the relay state is
    re-read afterwards."""
    try:
        yield bench_session
    finally:
        detail = bench_session.force_all_stop()
        relays = detail.get("relays_on")
        assert relays == [], f"relays still on after teardown: {detail}"


@pytest.fixture()
def cold_bench(bench):
    """``bench``, plus the guarantee that it STARTED cold.

    Thermal tests do not compose without this. Back to back, the second one
    begins on the first one's residual heat and measures a smaller step, a
    smaller gain and a shorter dead time -- all wrong in the same direction,
    and none of it visible in the result. The target is derived from the
    board's own cold junction at call time (BenchSession.wait_for_cooldown),
    so it tracks a room the owner reports at around 100 F rather than a
    hardcoded 25 C that would never be reached.

    The wait happens BEFORE the test body and the stop-everything teardown
    still happens after it: a test that asks for a cold start also gets the
    hard stop, because `bench` is what this wraps.

    KILNCTRL_BENCH_COOLDOWN_TARGET_C overrides the derived target for a
    session that wants an absolute gate; KILNCTRL_BENCH_COOLDOWN_TIMEOUT_S
    overrides the budget. Neither is needed for a normal run.
    """
    target = os.environ.get("KILNCTRL_BENCH_COOLDOWN_TARGET_C")
    timeout = os.environ.get("KILNCTRL_BENCH_COOLDOWN_TIMEOUT_S")
    kwargs = {}
    if target:
        kwargs["target_c"] = float(target)
    if timeout:
        kwargs["timeout_s"] = float(timeout)
    report = bench.wait_for_cooldown(**kwargs)
    print(f"\n[live] cold start: {report['hottest_c']:.2f} C "
          f"(target {report['target_c']:.2f} C, ambient ref {report['ambient_c']:.2f} C) "
          f"after {report['waited_s']:.0f} s")
    yield bench
