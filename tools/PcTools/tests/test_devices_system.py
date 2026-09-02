"""Tests for kilnctrl.devices_system's SYSTEM (task 6) wire encode/decode --
specifically the telemetry enable/disable subcommands (0x05/0x06) added to
wire the PC side up to telemetry_log.c's existing
telemetry_log_set_enabled()/telemetry_log_is_enabled().
"""
from __future__ import annotations

import pytest

from kilnctrl import devices
from kilnctrl.protocol import (
    SYSTEM_CMD_GET_TELEMETRY_ENABLED,
    SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED,
    SYSTEM_CMD_SET_TELEMETRY_ENABLED,
)


def test_system_set_telemetry_enabled_wire_shape():
    assert devices.system_set_telemetry_enabled(True) == bytes([SYSTEM_CMD_SET_TELEMETRY_ENABLED, 1])
    assert devices.system_set_telemetry_enabled(False) == bytes([SYSTEM_CMD_SET_TELEMETRY_ENABLED, 0])


def test_system_get_telemetry_enabled_wire_shape():
    assert devices.system_get_telemetry_enabled() == bytes([SYSTEM_CMD_GET_TELEMETRY_ENABLED])


@pytest.mark.parametrize("enabled", [True, False])
def test_parse_system_response_round_trips_telemetry_query(enabled):
    # Mirrors exactly what uart_bridge_system.c's GET_TELEMETRY_ENABLED case
    # sends back: byte0 = echoed subcommand, byte1 = the flag.
    reply = bytes([SYSTEM_CMD_GET_TELEMETRY_ENABLED, 1 if enabled else 0])
    subcommand, value = devices.parse_system_response(reply)
    assert subcommand == SYSTEM_CMD_GET_TELEMETRY_ENABLED
    assert value is enabled


def test_parse_system_response_still_accepts_watchdog_query():
    # The generalization from a single hardcoded subcommand to a set must
    # not regress the pre-existing GET_WATCHDOG_PANIC_DISABLED query.
    subcommand, value = devices.parse_system_response(bytes([SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED, 1]))
    assert subcommand == SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED
    assert value is True


def test_parse_system_response_rejects_unknown_subcommand():
    # A GET_TELEMETRY_ENABLED-shaped reply with a subcommand byte that
    # doesn't match any known query must be rejected, not silently accepted
    # as if it were one of the known ones -- this is the exact guard a
    # mutation that widened the accepted set (or dropped it entirely) would
    # break silently.
    with pytest.raises(devices.SystemResponseError):
        devices.parse_system_response(bytes([0xEE, 1]))


def test_parse_system_response_rejects_bad_flag_byte():
    with pytest.raises(devices.SystemResponseError):
        devices.parse_system_response(bytes([SYSTEM_CMD_GET_TELEMETRY_ENABLED, 2]))


def test_parse_system_response_rejects_wrong_length():
    with pytest.raises(devices.SystemResponseError):
        devices.parse_system_response(bytes([SYSTEM_CMD_GET_TELEMETRY_ENABLED]))
