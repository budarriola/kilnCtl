"""Unit tests for kilnctrl.coordinated_gpio_test -- pure logic, fake board
clients, no serial/SWD access. See that module's docstring for the
preconditions and deny-list this covers."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import pytest

from kilnctrl.coordinated_gpio_test import (
    DENY_PINS,
    GpioTestClients,
    GpioTestDeniedPin,
    GpioTestPreflight,
    guard_pin,
    run_coordinated_gpio_test,
    run_coordinated_gpio_test_lazy,
)


def _ok_preflight() -> GpioTestPreflight:
    return GpioTestPreflight(autotune_active=False, 
        safety_armed=False,
        profile_running_or_paused=False,
        profile_state_name="idle",
        ota_interlock_ok=True,
        ota_interlock_reason="ok",
        link_up=True,
    )


class FakeBoards:
    """A fake ESP + Pico pair that mirrors HARDWARE.md's predicted wiring:
    ESP GPIO4 -> Pico GPIO5 and Pico GPIO4 -> ESP GPIO5 both invert."""

    def __init__(self) -> None:
        self.esp_out = {}
        self.esp_modes = {}
        self.pico_out = {4: False}
        self.closed = False
        self.pico_reset = False
        self.esp_reset = False
        self.halted = False
        self.detached = []

    def esp_drive(self, gpio_num, level):
        self.esp_modes[gpio_num] = "output" if level is not None else "input"
        if level is not None:
            self.esp_out[gpio_num] = level

    def esp_read(self, gpio_num):
        if gpio_num == 4:
            # ESP4 mirrors Pico4 driving it, inverted.
            return not self.pico_out[4]
        if gpio_num == 5:
            # ESP5 mirrors Pico4 driving it too (Step B), inverted -- same
            # crossing HARDWARE.md predicts for GPIO5.
            return not self.pico_out[4]
        raise AssertionError(f"unexpected esp_read({gpio_num})")

    def _pico5(self):
        esp4 = self.esp_out.get(4, False)
        return not esp4

    def pico_halt(self):
        self.halted = True

    def pico_to_sio(self, gpio_num):
        self.detached.append(gpio_num)

    def pico_set_output(self, gpio_num, level):
        self.pico_out[gpio_num] = level

    def pico_read(self, gpio_num):
        if gpio_num == 5:
            return self._pico5()
        if gpio_num == 10:
            return False
        raise AssertionError(f"unexpected pico_read({gpio_num})")

    def pico_reset_run(self):
        self.pico_reset = True

    def esp_reset_run(self):
        self.esp_reset = True

    def esp_close(self):
        self.closed = True


def _clients(boards: FakeBoards, preflight: GpioTestPreflight) -> GpioTestClients:
    return GpioTestClients(
        get_preflight=lambda: preflight,
        esp_drive=boards.esp_drive,
        esp_read=boards.esp_read,
        esp_close=boards.esp_close,
        pico_halt=boards.pico_halt,
        pico_to_sio=boards.pico_to_sio,
        pico_set_output=boards.pico_set_output,
        pico_read=boards.pico_read,
        pico_reset_run=boards.pico_reset_run,
        esp_reset_run=boards.esp_reset_run,
        settle=lambda: None,
    )


def test_happy_path_all_pass_and_restores_both_boards():
    boards = FakeBoards()
    result = run_coordinated_gpio_test(_clients(boards, _ok_preflight()), confirm=True)

    assert not result.refused
    assert len(result.steps) == 4
    assert result.all_passed is True
    for step in result.steps:
        assert step.passed, step
    # Restoration always happens.
    assert boards.closed
    assert boards.pico_reset
    assert boards.esp_reset
    assert boards.halted
    assert sorted(boards.detached) == [4, 5, 10]


def test_step_b_mismatch_is_reported_as_failed_not_swallowed():
    boards = FakeBoards()

    # Break Step B only: Pico4 -> ESP4/5 no longer inverts, by making
    # pico_read('s ESP-side proxy) match instead of invert. We simulate this
    # by overriding esp_read to return the SAME level pico4 was set to
    # (should be inverted per HARDWARE.md) once Step B starts driving pico4.
    orig_esp_read = boards.esp_read

    def broken_esp_read(gpio_num):
        if gpio_num == 5 and boards.pico_out.get(4) is not None and boards.esp_modes.get(4) == "input":
            # Step B: report same-as-driven instead of inverted -- a wiring
            # mismatch a real board could exhibit.
            return boards.pico_out[4]
        return orig_esp_read(gpio_num)

    boards.esp_read = broken_esp_read

    result = run_coordinated_gpio_test(_clients(boards, _ok_preflight()), confirm=True)

    assert not result.refused
    step_a = [s for s in result.steps if s.label.startswith("stepA_")]
    step_b = [s for s in result.steps if s.label.startswith("stepB_")]
    assert all(s.passed for s in step_a)
    assert all(not s.passed for s in step_b)
    assert result.all_passed is False
    # Restoration still happens even though the result is a mismatch.
    assert boards.pico_reset and boards.esp_reset


def test_refuses_when_safety_armed():
    boards = FakeBoards()
    preflight = GpioTestPreflight(autotune_active=False, 
        safety_armed=True,  # ARMED -- must refuse
        profile_running_or_paused=False,
        profile_state_name="idle",
        ota_interlock_ok=True,
        ota_interlock_reason="ok",
        link_up=True,
    )
    result = run_coordinated_gpio_test(_clients(boards, preflight), confirm=True)

    assert result.refused
    assert any("ARMED" in r for r in result.refusal_reasons)
    assert result.steps == ()
    # No pin was ever touched or detached from firmware, and NEITHER board is
    # reset -- a refusal must be inert on the boards, since this exact
    # reason (ARMED, or a profile running/paused) means resetting either
    # processor here would abort a firing and trip S6a (Opus re-review of
    # 21383886).
    assert not boards.halted
    assert boards.detached == []
    assert not boards.pico_reset
    assert not boards.esp_reset
    # ...but the ESP probe client is still closed on this refusal path,
    # since by the time run_coordinated_gpio_test's own preflight check
    # runs, a caller may have already built a real (side-effectful) client
    # (Opus re-review of 430ba634) -- closing that PC-side socket is not a
    # board reset.
    assert boards.closed


def test_refuses_without_confirm():
    boards = FakeBoards()
    result = run_coordinated_gpio_test(_clients(boards, _ok_preflight()), confirm=False)

    assert result.refused
    assert any("confirm=True" in r for r in result.refusal_reasons)
    assert not boards.halted
    assert boards.detached == []


def test_denylisted_pin_refused_by_guard():
    assert 6 in DENY_PINS
    assert 9 in DENY_PINS
    with pytest.raises(GpioTestDeniedPin):
        guard_pin(6)
    with pytest.raises(GpioTestDeniedPin):
        guard_pin(9)
    # Pins the test actually uses are not deny-listed.
    for pin in (4, 5, 10):
        guard_pin(pin)  # must not raise


def test_lazy_refusal_never_builds_the_client():
    """Opus review of bdd06947: build_real_clients() constructs the real ESP
    ProbeClient eagerly (registers a task, starts a background thread on the
    shared link) -- if a caller builds it before checking preconditions,
    every refusal leaks one. run_coordinated_gpio_test_lazy must check
    preflight/confirm FIRST and never call the builder at all on a refusal."""
    boards = FakeBoards()
    armed_preflight = GpioTestPreflight(autotune_active=False, 
        safety_armed=True,  # ARMED -- must refuse
        profile_running_or_paused=False,
        profile_state_name="idle",
        ota_interlock_ok=True,
        ota_interlock_reason="ok",
        link_up=True,
    )
    build_calls = []

    def build_clients():
        build_calls.append(1)  # would be `probe.ProbeClient(link)` for real
        return _clients(boards, armed_preflight)

    result = run_coordinated_gpio_test_lazy(
        lambda: armed_preflight, build_clients, confirm=True)

    assert result.refused
    assert any("ARMED" in r for r in result.refusal_reasons)
    assert build_calls == []  # the client was never constructed
    assert not boards.closed  # nothing to close because nothing was opened
    assert not boards.halted


def test_lazy_confirm_false_never_builds_the_client():
    boards = FakeBoards()
    build_calls = []

    def build_clients():
        build_calls.append(1)
        return _clients(boards, _ok_preflight())

    result = run_coordinated_gpio_test_lazy(
        _ok_preflight, build_clients, confirm=False)

    assert result.refused
    assert build_calls == []


def test_lazy_happy_path_builds_and_runs():
    boards = FakeBoards()

    def build_clients():
        return _clients(boards, _ok_preflight())

    result = run_coordinated_gpio_test_lazy(_ok_preflight, build_clients, confirm=True)

    assert not result.refused
    assert result.all_passed is True
    assert boards.closed  # built, run, and cleaned up


def test_second_preflight_read_refusal_still_closes_client():
    """Opus re-review of 430ba634: run_coordinated_gpio_test's own internal
    clients.get_preflight() call (used when no `preflight=` snapshot is
    passed in) can refuse in a narrower window than a caller's own check --
    that refusal must still close the already-built ESP client, not just the
    happy-path finally block. It must NOT reset either board though (Opus
    re-review of 21383886): a refusal reached here can mean ARMED or a
    profile running/paused, and resetting a processor in that state would
    abort a firing and trip S6a -- a refusal must be inert on the boards."""
    boards = FakeBoards()
    armed_preflight = GpioTestPreflight(autotune_active=False, 
        safety_armed=True,
        profile_running_or_paused=False,
        profile_state_name="idle",
        ota_interlock_ok=True,
        ota_interlock_reason="ok",
        link_up=True,
    )
    result = run_coordinated_gpio_test(_clients(boards, armed_preflight), confirm=True)

    assert result.refused
    assert boards.closed
    assert not boards.pico_reset
    assert not boards.esp_reset
    # Never actually touched a pin though.
    assert not boards.halted
    assert boards.detached == []


def test_unknown_preflight_state_refuses_fail_safe():
    boards = FakeBoards()
    preflight = GpioTestPreflight(autotune_active=False, 
        safety_armed=None,  # could not be determined
        profile_running_or_paused=False,
        profile_state_name="idle",
        ota_interlock_ok=True,
        ota_interlock_reason="ok",
        link_up=True,
    )
    result = run_coordinated_gpio_test(_clients(boards, preflight), confirm=True)

    assert result.refused
    assert not boards.halted


def test_autotune_active_or_unread_refuses():
    import dataclasses
    ok = _ok_preflight()
    assert dataclasses.replace(ok, autotune_active=False).refusal_reasons() == []
    for val in (True, None):
        reasons = dataclasses.replace(ok, autotune_active=val).refusal_reasons()
        assert any("autotune" in r for r in reasons)
