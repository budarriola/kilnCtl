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
