"""Unit tests for tools/PcTools/scripts/bench_test_client.py (fakes only, no network)."""
from __future__ import annotations

import asyncio
import importlib.util
import sys
import types
from contextlib import asynccontextmanager
from pathlib import Path

import pytest

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "bench_test_client.py"

spec = importlib.util.spec_from_file_location("bench_test_client", SCRIPT_PATH)
client_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(client_mod)


class _Content:
    def __init__(self, text):
        self.text = text


class _Result:
    def __init__(self, texts, **flags):
        self.content = [_Content(t) for t in texts]
        for k, v in flags.items():
            setattr(self, k, v)


def _install_fakes(monkeypatch, streams, result):
    """Patch `mcp` / `mcp.client.streamable_http` in sys.modules."""
    seen = {}

    @asynccontextmanager
    async def fake_http(url):
        seen["url"] = url
        yield streams

    class FakeSession:
        def __init__(self, read, write):
            seen["rw"] = (read, write)

        async def __aenter__(self):
            return self

        async def __aexit__(self, *exc):
            return False

        async def initialize(self):
            seen["init"] = True

        async def call_tool(self, name, arguments):
            seen["call"] = (name, arguments)
            return result

    mcp = types.ModuleType("mcp")
    mcp.ClientSession = FakeSession
    client = types.ModuleType("mcp.client")
    http = types.ModuleType("mcp.client.streamable_http")
    http.streamable_http_client = fake_http
    mcp.client = client
    client.streamable_http = http
    monkeypatch.setitem(sys.modules, "mcp", mcp)
    monkeypatch.setitem(sys.modules, "mcp.client", client)
    monkeypatch.setitem(sys.modules, "mcp.client.streamable_http", http)
    return seen


def _run(args=("tool", {"a": 1}, 1234)):
    return asyncio.run(client_mod._call(*args))


@pytest.mark.parametrize("streams", [("R", "W"), ("R", "W", lambda: "sid")],
                         ids=["mcp2_two_tuple", "mcp1_three_tuple"])
def test_call_handles_both_stream_shapes(monkeypatch, streams):
    seen = _install_fakes(monkeypatch, streams, _Result(["a", "b"], is_error=False))
    assert _run() == "a\nb"
    assert seen["rw"] == ("R", "W")
    assert seen["url"] == "http://127.0.0.1:1234/mcp"
    assert seen["call"] == ("tool", {"a": 1})
    assert seen["init"] is True


@pytest.mark.parametrize("flags", [{"is_error": True}, {"isError": True}],
                         ids=["is_error_2x", "isError_1x"])
def test_error_result_raises_with_content_text(monkeypatch, flags):
    _install_fakes(monkeypatch, ("R", "W"), _Result(["boom", "detail"], **flags))
    with pytest.raises(RuntimeError) as ei:
        _run()
    assert str(ei.value) == "boom\ndetail"


def test_error_result_with_no_content_uses_default_message(monkeypatch):
    _install_fakes(monkeypatch, ("R", "W"), _Result([], is_error=True))
    with pytest.raises(RuntimeError) as ei:
        _run()
    assert str(ei.value) == "tool call reported an error"


@pytest.mark.skipif(sys.version_info < (3, 11), reason="ExceptionGroup is 3.11+")
def test_print_leaves_nested_group(capsys):
    inner = ExceptionGroup("inner", [ValueError("v"), KeyError("k")])
    outer = ExceptionGroup("outer", [inner, OSError("o")])
    client_mod._print_leaves(outer)
    err = capsys.readouterr().err
    assert "  cause: ValueError: v" in err
    assert "  cause: KeyError: 'k'" in err
    assert "  cause: OSError: o" in err
    assert err.count("cause:") == 3


def test_print_leaves_plain_exception_prints_nothing(capsys):
    client_mod._print_leaves(RuntimeError("x"))
    cap = capsys.readouterr()
    assert cap.err == "" and cap.out == ""
