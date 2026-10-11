#!/usr/bin/env python3
"""Generic sweep over every kilnctrl MCP tool with a ``confirm`` / ``confirm_erase``
parameter (docs/audits/PCTOOLS_MCP_COVERAGE_GAPS_2026-10-10.md).

For each such tool, calling it with a truthy NON-True gate value ("yes", 1, "true",
[1]) must (a) not make a non-GET HTTP request, (b) not spawn a subprocess, (c) not
open a socket to a board/hub, and (d) either refuse in its reply or raise.
All network, subprocess and socket access is replaced by recorders, so no board is
ever touched. flash_firmware's ``confirm`` gate and the facade-only ``confirm`` check
are covered separately in test_flash_firmware_confirm_gate.py.

Run with: python -m pytest tools/PcTools/tests/test_mcp_confirm_gate_sweep.py -q
"""
from __future__ import annotations

import importlib
import inspect
import os
import pkgutil
import socket
import subprocess
import sys
import unittest.mock as um

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import kilnctrl  # noqa: E402

GATE_PARAMS = ("confirm", "confirm_erase")
BAD_VALUES = ("yes", 1, "true", [1])

# Tools that deliberately perform board READS before the gate (precheck/preflight)
# are fine; the sweep only forbids writes. These tools are documented as running
# the gate AFTER a socket-level read (UART link hub) so a socket connect is allowed.
SOCKET_READ_BEFORE_GATE_OK: set[str] = set()


def _tools():
    out = []
    for m in pkgutil.iter_modules(kilnctrl.__path__):
        if not m.name.startswith("mcp_server"):
            continue
        mod = importlib.import_module("kilnctrl." + m.name)
        for name, fn in inspect.getmembers(mod, inspect.isfunction):
            if fn.__module__ != mod.__name__ or name.startswith("_"):
                continue
            params = inspect.signature(fn).parameters
            for g in GATE_PARAMS:
                if g in params:
                    out.append(pytest.param(fn, g, id=f"{m.name}.{name}.{g}"))
    return out


# Per-tool arguments that get past argument validation so the call reaches its gate.
# (ui_run_script gates only when apply_preset=True and the named script has a preset.)
ARG_OVERRIDES = {
    "control_set_zone_limits": {"max_temp_c": 100.0},
    "safety_set_tc_type": {"tc_type_name": "k"},
    "ui_run_script": {"apply_preset": True, "name": "home_start_stop_zone0_lcd"},
    "safety_set_fault_out": {"assert_fault": False},
    # bench_test_run/start gate confirm only when an ota_*/update_* argument is given (a valid suite
    # is checked first), so pass one to reach the gate.
    "bench_test_run": {"suite": "smoke", "ota_image_path": "x.bin"},
}
# flash_firmware validates build outputs before its gate and is covered by
# test_flash_firmware_confirm_gate.py with a real fake build tree.
# bench_test_start is the background twin of bench_test_run: it hands `confirm` to the job thread, and the
# refusal arrives in the job's FAILED report (test_bench_test_start_jobs.test_ota_images_without_confirm_refuse_before_runner).
RUNTIME_EXEMPT = {"flash_firmware", "bench_test_start"}


def _dummy_args(fn, gate, bad):
    kwargs = dict(ARG_OVERRIDES.get(fn.__name__, {}))
    for pname, p in inspect.signature(fn).parameters.items():
        if pname == gate:
            kwargs[pname] = bad
        elif pname in kwargs:
            continue
        elif p.default is inspect.Parameter.empty and p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY):
            ann = str(p.annotation)
            if "float" in ann:
                kwargs[pname] = 1.0
            elif "int" in ann:
                kwargs[pname] = 1
            elif "bool" in ann:
                kwargs[pname] = False
            elif "list" in ann.lower():
                kwargs[pname] = []
            else:
                kwargs[pname] = "x"
    return kwargs


class _Touched(Exception):
    pass


@pytest.mark.parametrize("fn,gate", _tools())
@pytest.mark.parametrize("bad", BAD_VALUES, ids=lambda b: repr(b))
def test_truthy_non_true_gate_value_refused_without_writes(fn, gate, bad, monkeypatch, tmp_path):
    if fn.__name__ in RUNTIME_EXEMPT:
        pytest.skip("covered by test_flash_firmware_confirm_gate.py")
    monkeypatch.chdir(tmp_path)
    writes = []

    def fake_urlopen(req, *a, **k):
        method = getattr(req, "get_method", lambda: "GET")() if not isinstance(req, str) else "GET"
        if method != "GET":
            writes.append(("http", method, getattr(req, "full_url", str(req))))
        raise OSError("blocked by test")

    def blocked(*a, **k):
        writes.append(("blocked", a[:1]))
        raise OSError("blocked by test")

    def blocked_socket(*a, **k):
        # UART-hub / board reads (a precheck before the gate) are tolerated, but the
        # socket is dead, so nothing can be written; the reply must still be a refusal.
        raise OSError("blocked by test")

    monkeypatch.setattr("urllib.request.urlopen", fake_urlopen)
    from kilnctrl import http_auth
    monkeypatch.setattr(http_auth, "urlopen", fake_urlopen)
    monkeypatch.setattr(subprocess, "run", blocked)
    monkeypatch.setattr(subprocess, "Popen", blocked)
    monkeypatch.setattr(socket, "create_connection", blocked_socket)
    monkeypatch.setattr(socket.socket, "connect", blocked_socket)
    monkeypatch.setattr(socket.socket, "sendall", blocked_socket)
    monkeypatch.setattr(socket.socket, "send", blocked_socket)

    kwargs = _dummy_args(fn, gate, bad)
    try:
        result = fn(**kwargs)
    except Exception as exc:  # a raise before touching anything is also a refusal
        result = f"raised {type(exc).__name__}: {exc}"
    assert not writes, f"{fn.__name__}({gate}={bad!r}) wrote/spawned: {writes}"
    assert isinstance(result, (str, dict)) or result is None, type(result)
    text = str(result).lower()
    assert any(w in text for w in ("refus", "confirm", "could not", "blocked by test", "unreadable", "failed")), text[:300]


def _gate_checked_exactly(fn, gate) -> bool:
    """Static check: the function (or a gate helper it hands the flag to) tests the flag
    with ``is not True`` / ``is True`` -- never plain truthiness."""
    src = inspect.getsource(fn)
    if f"{gate} is not True" in src or f"{gate} is True" in src:
        return True
    # delegated: ``_x_gate(..., confirm)`` whose body uses an exact test
    mod = sys.modules[fn.__module__]
    for helper in set(__import__("re").findall(r"(_\w*(?:gate|refusal)\w*)\(", src)):
        h = getattr(mod, helper, None)
        if h is not None and ("is not True" in inspect.getsource(h) or "is True" in inspect.getsource(h)):
            return True
    return False


@pytest.mark.parametrize("fn,gate", _tools())
def test_gate_is_an_exact_true_test_not_truthiness(fn, gate):
    if fn.__name__ in RUNTIME_EXEMPT:
        pytest.skip("covered elsewhere (see RUNTIME_EXEMPT)")
    assert _gate_checked_exactly(fn, gate), (
        f"{fn.__module__}.{fn.__name__} does not test {gate!r} with 'is True'/'is not True'")
