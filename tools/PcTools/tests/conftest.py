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
