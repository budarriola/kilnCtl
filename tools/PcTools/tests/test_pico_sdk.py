"""Tests for mcpkit.pico_sdk.resolve_pico_sdk_path -- see that module's
docstring for the resolution order this locks down."""

import os

import pytest

from mcpkit.pico_sdk import (
    DEFAULT_PICO_SDK_PATH,
    PicoSdkNotFoundError,
    resolve_pico_sdk_path,
)


def test_env_var_wins_when_set():
    env = {"PICO_SDK_PATH": r"D:\somewhere\else"}
    assert resolve_pico_sdk_path(env) == r"D:\somewhere\else"


def test_env_var_wins_even_if_default_would_also_resolve(tmp_path, monkeypatch):
    # Point the "bench default" check at a real fake SDK dir by monkeypatching
    # the module constant, then confirm an explicit env var still wins.
    fake_sdk = tmp_path / "pico-sdk"
    fake_sdk.mkdir()
    (fake_sdk / "pico_sdk_init.cmake").write_text("# fake\n")
    monkeypatch.setattr("mcpkit.pico_sdk.DEFAULT_PICO_SDK_PATH", str(fake_sdk))

    env = {"PICO_SDK_PATH": r"D:\somewhere\else"}
    assert resolve_pico_sdk_path(env) == r"D:\somewhere\else"


def test_falls_back_to_default_when_it_looks_like_a_real_sdk(tmp_path, monkeypatch):
    fake_sdk = tmp_path / "pico-sdk"
    fake_sdk.mkdir()
    (fake_sdk / "pico_sdk_init.cmake").write_text("# fake\n")
    monkeypatch.setattr("mcpkit.pico_sdk.DEFAULT_PICO_SDK_PATH", str(fake_sdk))

    assert resolve_pico_sdk_path({}) == str(fake_sdk)


def test_raises_naming_both_locations_when_neither_resolves(tmp_path, monkeypatch):
    empty_dir = tmp_path / "not-an-sdk"
    empty_dir.mkdir()
    monkeypatch.setattr("mcpkit.pico_sdk.DEFAULT_PICO_SDK_PATH", str(empty_dir))

    with pytest.raises(PicoSdkNotFoundError) as excinfo:
        resolve_pico_sdk_path({})
    message = str(excinfo.value)
    assert "PICO_SDK_PATH" in message
    assert empty_dir.name in message


def test_default_env_uses_os_environ(monkeypatch):
    monkeypatch.delenv("PICO_SDK_PATH", raising=False)
    # Whatever the real bench machine has (present or not), this must not
    # raise a type error or crash -- it either returns the real env var, the
    # real bench default, or a clean PicoSdkNotFoundError.
    try:
        result = resolve_pico_sdk_path()
        assert isinstance(result, str) and result
    except PicoSdkNotFoundError as exc:
        assert DEFAULT_PICO_SDK_PATH in str(exc)
