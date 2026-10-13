"""UnitTestFixture relay-control tools (PCF8575, task 7 on that board's OWN
UART link -- a separate physical board from the main kilnCtl controller).

See ``fixture.py`` for the transport/relay-map details and
``docs/UNIT_TEST_FIXTURE_PLAN.md`` for the owner decision and scope: this is
a control-only surface (flip relays through the I/O expanders) with nothing
else from the old fixture firmware (AD9833/MCP4728/SSD1306) touched.

Registered lazily: the fixture client only opens its own serial port on
first use, since a bench session may not have the fixture attached at all
and must not block on it.
"""
from __future__ import annotations

import threading
from typing import Optional

from .fixture import DEFAULT_RELAY_MAP, FixtureClient, FixtureError
from . import mcp_server_core as _core

_fixture_lock = threading.Lock()
_fixture: Optional[FixtureClient] = None


def _get_fixture() -> FixtureClient:
    global _fixture
    with _fixture_lock:
        if _fixture is None:
            _fixture = FixtureClient()
        if not _fixture.is_connected:
            _fixture.connect()
        return _fixture


def _close_fixture() -> None:
    """Called from mcp_server.py's shutdown -- de-energizes and releases the
    port if the fixture was ever opened this session."""
    global _fixture
    with _fixture_lock:
        if _fixture is not None:
            _fixture.close()
            _fixture = None


@_core._tool()
def fixture_list_relays() -> str:
    """List every relay name the fixture control surface knows about.

    Names are placeholders (``U4:P00`` .. ``U5:P15``) until the fixture's
    relay board is actually designed and wired -- see fixture.py's module
    docstring. Does not require a connection.
    """
    return ", ".join(sorted(DEFAULT_RELAY_MAP))


@_core._tool()
def fixture_set_relay(name: str, on: bool, confirm: bool = False) -> str:
    """Energize (``on=True``) or de-energize one fixture relay by name.

    Connects to the fixture on first use (own USB-UART bridge, separate from
    the main kilnCtl board -- see docs/UNIT_TEST_FIXTURE_PLAN.md "PC
    connection identity"). A bad relay name is refused before anything is
    sent.

    Requires confirm=True (exactly): a fixture relay can cut a board's supply
    or short a pin. The pin state is read back afterwards and a mismatch is
    reported as FAILED.
    """
    if confirm is not True:
        return "error: fixture_set_relay refused without confirm=True -- a fixture relay can cut a board's supply or short a pin"
    try:
        fixture = _get_fixture()
        fixture.set_relay(name, on)
    except FixtureError as exc:
        return f"error: {exc}"
    try:
        got = fixture.get_relays().get(name)
    except Exception:  # noqa: BLE001 - the write already happened
        return f"FAILED - {name} commanded {'on' if on else 'off'} but the read-back failed; state UNVERIFIED"
    if got is None:
        return f"FAILED - {name} commanded {'on' if on else 'off'} but the relay is missing from the read-back; state UNVERIFIED"
    if bool(got) != bool(on):
        return f"FAILED - {name} commanded {'on' if on else 'off'} but reads back {'energized' if got else 'de-energized'}"
    return f"ok - {name} {'energized' if on else 'de-energized'}"


@_core._tool()
def fixture_get_relays() -> str:
    """Read back every mapped relay's live pin state from the fixture.

    Returns real device data (a query), one line per relay:
    ``name: energized`` / ``name: de-energized``.
    """
    try:
        fixture = _get_fixture()
        states = fixture.get_relays()
    except FixtureError as exc:
        return f"error: {exc}"
    return "\n".join(
        f"{name}: {'energized' if on else 'de-energized'}" for name, on in sorted(states.items())
    )


@_core._tool()
def fixture_all_off() -> str:
    """De-energize every fixture relay. Safe to call whether or not anything
    is currently energized; also runs automatically on connect/disconnect."""
    try:
        fixture = _get_fixture()
        fixture.all_off()
    except FixtureError as exc:
        return f"error: {exc}"
    return "ok - all fixture relays de-energized"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
