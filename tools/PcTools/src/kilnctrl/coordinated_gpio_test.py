#!/usr/bin/env python3
"""Coordinated two-board GPIO test -- `tools/PcTools/TODO.md` capability 1c,
implementing `firmware/SaftyFW/docs/HARDWARE.md` section 1, Steps A and B.

Verifies the isolated safety-link UART wiring (ESP GPIO4/GPIO5 <-> Pico
GPIO4/GPIO5) by direct pin control, independent of both firmwares' UART
peripherals -- this is the only test in the project that proves the crossing
rather than assuming it from the schematic.

Each processor is reached by a path that is *not* the link under test:
  - ESP:  GPIO_PROBE task (12) over its USB-serial PC link (`kilnctrl.probe`)
  - Pico: raw RP2040 SIO/IO_BANK0 register pokes over SWD (`kilnctrl.debug_probe`)

This module is the importable core, split out of
`tools/PcTools/scripts/coordinated_gpio_test.py` (now a thin CLI wrapper) so
it can be (a) unit-tested with fake board clients and (b) reached through the
MCP facade as a single `coordinated_gpio_test` tool
(`mcp_server_coordinated_gpio_test.py`) -- previously a bench agent could not
run this test at all without shelling out to the standalone script directly,
bypassing every precondition a normal tool call gets.

DENY-LISTED PINS -- never driven or read by this module, on EITHER side,
regardless of caller: GPIO6 has two different safety-relevant meanings
depending on which processor's pin is meant -- on the ESP it is the Fault
line output, already deny-listed inside `gpio_probe.c` on the firmware side
(a debug tool that can drive it could silently misrepresent the main
controller's health to the safety processor); on the Pico it is
`SAFTYFW_PIN_RELAY` (`firmware/SaftyFW/src/board/board_pins.h:28`), the
safety relay/heat-enable drive to Q4/K4 -- a debug tool that can drive it
could energize the heaters directly. GPIO9 is the E-stop input, read over
SWD on the Pico side -- driving or overriding it is a hazard this test has
no business taking.
:func:`guard_pin` enforces this before every single pin write in
:func:`run_coordinated_gpio_test`, as defense in depth on top of the
preconditions below (which already require the safety chain to be sane
before any pin is touched at all).

Step C (the fault line, ESP GPIO6) is deliberately NOT implemented here --
see the deny-list note above and HARDWARE.md's own "undisputed" comment on
that crossing.

PRECONDITIONS (enforced by :func:`run_coordinated_gpio_test`, before any pin
write): safety not ARMED, no profile running or paused, the OTA interlock
reporting idle, and the safety link already confirmed up. All four are
required because this test intentionally detaches GPIO4/5/10 from both
firmwares' own UART peripherals for its duration -- doing that while the
safety relay could be live, a firing is in progress, an OTA/rollback is
mid-flight, or the link was never confirmed healthy in the first place would
be indistinguishable from a real safety-link outage to anything watching.
Any precondition read failure (an exception, or the underlying client
reporting "unknown") is treated as a refusal, never as "assume it's fine".
The caller must also pass `confirm=True`; both boards are power-cycled/reset
at the end regardless of how the run finished (matching the original
script's `finally` block).

Both boards must be powered (main board 12V *and* 12V_Safty) and connected:
ESP over its normal USB-serial link, Pico over its SWD debug probe.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Callable, Optional

# --- RP2040 register map (RP2040 datasheet ch. 2.3 SIO, ch. 2.19 IO_BANK0) --
IO_BANK0_BASE = 0x40014000
SIO_BASE = 0xD0000000

SIO_GPIO_IN = SIO_BASE + 0x004
SIO_GPIO_OUT_SET = SIO_BASE + 0x014
SIO_GPIO_OUT_CLR = SIO_BASE + 0x018
SIO_GPIO_OE_SET = SIO_BASE + 0x024
SIO_GPIO_OE_CLR = SIO_BASE + 0x028

GPIO_FUNC_SIO = 5

#: Never driven or read by this module on either side. See module docstring.
DENY_PINS = frozenset({6, 9})


def gpio_ctrl_addr(gpio_num: int) -> int:
    return IO_BANK0_BASE + 8 * gpio_num + 4


class GpioTestDeniedPin(Exception):
    """Raised by :func:`guard_pin` -- an attempt to touch a deny-listed pin
    (GPIO6, the ESP fault line, or GPIO9, the E-stop input) was refused
    before any register access happened."""


def guard_pin(gpio_num: int) -> None:
    """Refuse `gpio_num` if it is in :data:`DENY_PINS`. Called immediately
    before every pin write/read this module performs."""
    if gpio_num in DENY_PINS:
        reason = (
            "ESP fault line output / Pico SAFTYFW_PIN_RELAY (safety relay, "
            "heat-enable drive to Q4/K4)" if gpio_num == 6 else "E-stop input"
        )
        raise GpioTestDeniedPin(
            f"refusing to touch GPIO{gpio_num} -- deny-listed ({reason}), never "
            f"driven or read by coordinated_gpio_test regardless of caller"
        )


# ---------------------------------------------------------------------------
# Preflight -- read-only precondition snapshot
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class GpioTestPreflight:
    """A snapshot of the four preconditions, already reduced to
    tri-state booleans (`True`/`False`/`None` for "could not determine").
    `None` is never treated as satisfied -- see :meth:`refusal_reasons`."""

    safety_armed: Optional[bool]
    profile_running_or_paused: Optional[bool]
    profile_state_name: str
    ota_interlock_ok: Optional[bool]
    ota_interlock_reason: str
    link_up: Optional[bool]

    def refusal_reasons(self) -> "list[str]":
        reasons: "list[str]" = []
        if self.safety_armed is not False:
            reasons.append(
                f"safety relay is ARMED or its state could not be confirmed "
                f"(safety_armed={self.safety_armed!r}) -- refusing to detach "
                f"GPIO4/5/10 from firmware while the safety chain could be live"
            )
        if self.profile_running_or_paused is not False:
            reasons.append(
                f"a profile is running/paused or its state could not be "
                f"confirmed (profile_state={self.profile_state_name!r}) -- "
                f"stop any firing first"
            )
        if self.ota_interlock_ok is not True:
            reasons.append(
                f"OTA interlock is not idle or could not be confirmed: "
                f"{self.ota_interlock_reason}"
            )
        if self.link_up is not True:
            reasons.append(
                f"safety link was not confirmed up before the test "
                f"(link_up={self.link_up!r}) -- this test can only tell you "
                f"the raw wiring is bad by starting from a link already known "
                f"good"
            )
        return reasons


# ---------------------------------------------------------------------------
# Step measurements / result
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class StepMeasurement:
    """One drive-and-read measurement. `expected` is HARDWARE.md's
    prediction (both crossings invert); `observed` is `None` only if the
    read itself raised, which :func:`run_coordinated_gpio_test` does not
    catch (a read failure mid-test aborts the whole run, same as the
    original script)."""

    label: str
    expected: bool
    observed: bool

    @property
    def passed(self) -> bool:
        return self.observed == self.expected


@dataclass(frozen=True)
class GpioTestResult:
    refused: bool
    refusal_reasons: "tuple[str, ...]" = ()
    steps: "tuple[StepMeasurement, ...]" = ()
    #: Unasserted baseline reads taken alongside each step (Pico GPIO10 in
    #: Step A, ESP GPIO4 in Step B) -- diagnostic only, never pass/fail, but
    #: worth keeping visible on a mismatch rather than reading-and-discarding
    #: them as the pre-fix version did.
    diagnostics: "tuple[str, ...]" = ()

    @property
    def all_passed(self) -> Optional[bool]:
        if self.refused or not self.steps:
            return None
        return all(s.passed for s in self.steps)

    def describe(self) -> str:
        if self.refused:
            lines = ["REFUSED -- coordinated_gpio_test did not touch any pin:"]
            lines.extend(f"  - {r}" for r in self.refusal_reasons)
            return "\n".join(lines)
        lines = ["=== Coordinated two-board GPIO test result ==="]
        for s in self.steps:
            verdict = "PASS" if s.passed else "FAIL"
            lines.append(
                f"  [{verdict}] {s.label}: expected={s.expected!r} observed={s.observed!r}"
            )
        lines.append(
            "RESULT: "
            + ("ALL PASS -- matches HARDWARE.md" if self.all_passed
               else "MISMATCH -- do not paper over this, re-check HARDWARE.md section 1")
        )
        if self.diagnostics:
            lines.append("Diagnostic baseline reads (not pass/fail):")
            lines.extend(f"  - {d}" for d in self.diagnostics)
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# Injected clients -- real callables talk to the boards; tests supply fakes.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class GpioTestClients:
    get_preflight: Callable[[], GpioTestPreflight]
    esp_drive: Callable[[int, "Optional[bool]"], None]
    esp_read: Callable[[int], bool]
    esp_close: Callable[[], None]
    pico_halt: Callable[[], None]
    pico_to_sio: Callable[[int], None]
    pico_set_output: Callable[[int, bool], None]
    pico_read: Callable[[int], bool]
    pico_reset_run: Callable[[], None]
    esp_reset_run: Callable[[], None]
    #: Settle delay after a drive, before the corresponding read -- the
    #: original script slept 0.05s here for the optocoupler/level-shift to
    #: settle. Injected (not a bare `time.sleep` call) so tests run instant.
    settle: Callable[[], None] = lambda: None


def check_preflight_refusal(preflight: GpioTestPreflight, confirm: bool) -> "tuple[str, ...]":
    """The single source of truth for whether a run should be refused, given
    an already-read preflight snapshot and the caller's `confirm` flag.
    Deliberately callable BEFORE any client (real or fake) is constructed --
    see :func:`run_coordinated_gpio_test_lazy`'s docstring for why this
    matters: constructing the real ESP client (`probe.ProbeClient`) has side
    effects (registers a task and starts a background thread on the shared
    link), so a caller must be able to decide refusal without ever building
    one."""
    reasons = list(preflight.refusal_reasons())
    if not confirm:
        reasons.append("confirm=True was not passed -- refusing to drive any pin")
    return tuple(reasons)


def run_coordinated_gpio_test(
    clients: GpioTestClients, confirm: bool = False,
    preflight: "Optional[GpioTestPreflight]" = None,
) -> GpioTestResult:
    """Run Steps A and B, after checking preconditions. Never touches a pin
    if `confirm` is not `True`, or if any precondition in
    :meth:`GpioTestPreflight.refusal_reasons` fails. Restores both boards
    (reset to run mode) in a `finally` regardless of how the run ends, same
    as the original script.

    NOTE: by the time this function is called, `clients` (and, for
    `build_real_clients`, the real `probe.ProbeClient`) has ALREADY been
    constructed -- so this function's own refusal check is a second,
    defense-in-depth check, not the only one. A caller that owns something
    with construction side effects (like the real ESP client) MUST call
    :func:`check_preflight_refusal` itself first and skip building clients
    entirely on refusal -- see :func:`run_coordinated_gpio_test_lazy`, which
    both the CLI and the MCP tool use for exactly this reason (Opus review of
    bdd06947 found the eager-construction leak this guards against).

    `preflight`: pass the already-read snapshot through (as
    `run_coordinated_gpio_test_lazy` does) rather than letting this function
    call `clients.get_preflight()` a second time -- a second, independent
    read can itself refuse in a narrower window than the caller's own check,
    which is the same leak shape the note above describes, just smaller
    (Opus re-review of 430ba634)."""
    if preflight is None:
        preflight = clients.get_preflight()
    reasons = check_preflight_refusal(preflight, confirm)
    if reasons:
        # `clients` (and, for build_real_clients, the real ProbeClient) may
        # already exist by this point -- close it here too, not just in the
        # `finally` below, so this refusal path never leaks the ESP client
        # (Opus re-review of 430ba634: this second preflight read, narrower
        # than the caller's own, was the same leak shape). Deliberately
        # NEVER pico_reset_run()/esp_reset_run() here, unlike the finally
        # below: the refusal reasons that reach this branch include "safety
        # relay is ARMED" and "a profile is running/paused", and resetting
        # either processor mid-firing would abort it and trip S6a. A refusal
        # must be inert on both boards -- closing the PC-side probe socket
        # is not a board reset (Opus re-review of 21383886).
        clients.esp_close()
        return GpioTestResult(refused=True, refusal_reasons=reasons)

    steps: "list[StepMeasurement]" = []
    diagnostics: "list[str]" = []
    try:
        clients.pico_halt()
        for pin in (4, 5, 10):
            guard_pin(pin)
            clients.pico_to_sio(pin)

        # --- Step A: ESP drives GPIO4, Pico reads GPIO5 (+GPIO10 baseline) --
        guard_pin(5)
        clients.esp_drive(5, None)  # ESP GPIO5 input, own RX -- expect no change
        for level in (True, False):
            guard_pin(4)
            clients.esp_drive(4, level)
            clients.settle()
            guard_pin(5)
            pico5 = clients.pico_read(5)
            guard_pin(10)
            pico10 = clients.pico_read(10)  # baseline read, not asserted on
            guard_pin(5)
            esp5_ownrx = clients.esp_read(5)
            diagnostics.append(
                f"stepA esp4={level!r}: pico_gpio10={pico10!r} esp5(own_rx)={esp5_ownrx!r}")
            steps.append(StepMeasurement(
                label=f"stepA_esp4_{level}_pico5", expected=not level, observed=pico5))
        clients.esp_drive(4, False)  # leave idle low afterward

        # --- Step B: Pico drives GPIO4, ESP reads GPIO4 and GPIO5 ----------
        guard_pin(4)
        clients.esp_drive(4, None)  # ESP GPIO4 input now
        guard_pin(5)
        clients.esp_drive(5, None)  # ESP GPIO5 input (own RX)
        for level in (True, False):
            guard_pin(4)
            clients.pico_set_output(4, level)
            clients.settle()
            guard_pin(4)
            esp4 = clients.esp_read(4)
            guard_pin(5)
            esp5 = clients.esp_read(5)
            diagnostics.append(f"stepB pico4={level!r}: esp_gpio4={esp4!r}")
            steps.append(StepMeasurement(
                label=f"stepB_pico4_{level}_esp5", expected=not level, observed=esp5))
        clients.pico_set_output(4, False)  # leave idle low
    finally:
        clients.esp_close()
        clients.pico_reset_run()
        clients.esp_reset_run()

    return GpioTestResult(
        refused=False, refusal_reasons=(), steps=tuple(steps),
        diagnostics=tuple(diagnostics))


def run_coordinated_gpio_test_lazy(
    get_preflight: Callable[[], GpioTestPreflight],
    build_clients: Callable[[], GpioTestClients],
    confirm: bool = False,
) -> GpioTestResult:
    """Preferred entry point for any caller whose real client construction
    has side effects (the real ESP client registers a GPIO_PROBE task and
    starts a background thread on the shared link the moment it's built).
    Reads the preflight and checks refusal FIRST, via
    :func:`check_preflight_refusal`, and calls `build_clients()` -- which
    does the actual, side-effectful construction -- only once it's already
    known the run will proceed. On refusal, `build_clients` is never
    invoked, so nothing is ever left open to close. Both
    `scripts/coordinated_gpio_test.py` and `mcp_server_coordinated_gpio_test.py`
    use this instead of calling :func:`run_coordinated_gpio_test` directly
    with an eagerly-built `GpioTestClients` (see that function's own
    docstring note -- Opus review of bdd06947)."""
    preflight = get_preflight()
    reasons = check_preflight_refusal(preflight, confirm)
    if reasons:
        return GpioTestResult(refused=True, refusal_reasons=reasons)

    clients = build_clients()
    # Pass the already-read snapshot through instead of letting
    # run_coordinated_gpio_test call clients.get_preflight() again -- a
    # second, independent read could itself refuse in the narrow window
    # between here and there, leaking the client we just built for exactly
    # the same reason this function exists (Opus re-review of 430ba634).
    return run_coordinated_gpio_test(clients, confirm=confirm, preflight=preflight)


# ---------------------------------------------------------------------------
# Real (non-test) client wiring, used by both the CLI script and the MCP
# tool. Kept here (not duplicated in each caller) so both reach the boards
# identically.
# ---------------------------------------------------------------------------
def real_pico_gpio_read(peer: str, gpio_num: int, debug_probe_module) -> bool:
    """Shared parse of `debug_probe.read_memory`'s "MEMRD 0xADDR 0xVALUE"
    text reply -- see the original script's own note on why this parses
    that text rather than mdw's raw display output."""
    ok, out = debug_probe_module.read_memory(peer, SIO_GPIO_IN, count=1, width=32)
    if not ok:
        raise RuntimeError(f"pico gpio_in read failed: {out}")
    match = re.search(r"MEMRD\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", out)
    if not match:
        raise RuntimeError(f"could not parse read_memory output for gpio_in: {out!r}")
    value = int(match.group(1), 16)
    return bool(value & (1 << gpio_num))


def build_real_clients(link, debug_probe_module, probe_module,
                        get_preflight: Callable[[], GpioTestPreflight]) -> GpioTestClients:
    """Wire up a :class:`GpioTestClients` that actually talks to both boards
    over `link` (the ESP probe) and SWD (`debug_probe_module`, normally
    `kilnctrl.debug_probe`). `probe_module` is normally `kilnctrl.probe`.
    Kept as a function (not a class) so both the standalone script and the
    MCP tool build the identical real wiring from the identical few lines."""
    import time

    #: The original script's explicit per-call timeout -- ProbeClient's own
    #: default (`probe.DEFAULT_REPLY_TIMEOUT_S`, 2.0s) is too tight for this
    #: test's drive/read round trips; restored here after the refactor
    #: silently dropped it (Opus review of bdd06947).
    _ESP_TIMEOUT_S = 5.0

    esp = probe_module.ProbeClient(link)
    peer_pico = debug_probe_module.PEER_PICO
    peer_esp = debug_probe_module.PEER_ESP

    def esp_drive(gpio_num: int, level: "Optional[bool]") -> None:
        if level is None:
            esp.set_mode(gpio_num, probe_module.MODE_INPUT, timeout=_ESP_TIMEOUT_S)
            return
        esp.set_mode(gpio_num, probe_module.MODE_OUTPUT, timeout=_ESP_TIMEOUT_S)
        esp.write(gpio_num, level, timeout=_ESP_TIMEOUT_S)

    def pico_to_sio(gpio_num: int) -> None:
        ok, out = debug_probe_module.write_memory(
            peer_pico, gpio_ctrl_addr(gpio_num), GPIO_FUNC_SIO, width=32)
        if not ok:
            raise RuntimeError(f"pico gpio{gpio_num} funcsel->SIO failed: {out}")
        ok, out = debug_probe_module.write_memory(
            peer_pico, SIO_GPIO_OE_CLR, 1 << gpio_num, width=32)
        if not ok:
            raise RuntimeError(f"pico gpio{gpio_num} OE clear failed: {out}")

    def pico_set_output(gpio_num: int, level: bool) -> None:
        ok, out = debug_probe_module.write_memory(
            peer_pico, SIO_GPIO_OE_SET, 1 << gpio_num, width=32)
        if not ok:
            raise RuntimeError(f"pico gpio{gpio_num} OE set failed: {out}")
        addr = SIO_GPIO_OUT_SET if level else SIO_GPIO_OUT_CLR
        ok, out = debug_probe_module.write_memory(peer_pico, addr, 1 << gpio_num, width=32)
        if not ok:
            raise RuntimeError(
                f"pico gpio{gpio_num} drive {'high' if level else 'low'} failed: {out}")

    def pico_read(gpio_num: int) -> bool:
        return real_pico_gpio_read(peer_pico, gpio_num, debug_probe_module)

    return GpioTestClients(
        get_preflight=get_preflight,
        esp_drive=esp_drive,
        esp_read=lambda gpio_num: esp.read(gpio_num, timeout=_ESP_TIMEOUT_S),
        esp_close=esp.close,
        pico_halt=lambda: debug_probe_module.halt(peer_pico),
        pico_to_sio=pico_to_sio,
        pico_set_output=pico_set_output,
        pico_read=pico_read,
        pico_reset_run=lambda: debug_probe_module.reset(peer_pico, mode="run"),
        esp_reset_run=lambda: debug_probe_module.reset(peer_esp, mode="run"),
        settle=lambda: time.sleep(0.05),
    )
