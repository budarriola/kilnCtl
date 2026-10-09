#!/usr/bin/env python3
"""Thin CLI over `kilnctrl.coordinated_gpio_test` -- see that module's
docstring for the full explanation of what this test does, its
preconditions, and the deny-listed pins (GPIO6, GPIO9) it never touches.

This script is now a wrapper only: all the logic (preflight, step
sequencing, deny-list guard) lives in `kilnctrl.coordinated_gpio_test` so it
can be unit-tested with fakes and reached through the MCP facade as the
`coordinated_gpio_test` tool (`mcp_server_coordinated_gpio_test.py`).

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/coordinated_gpio_test.py
"""

from __future__ import annotations

import sys

sys.path.insert(0, __file__.rsplit("scripts", 1)[0] + "src")

from kilnctrl import debug_probe, probe
from kilnctrl.coordinated_gpio_test import (
    GpioTestPreflight,
    build_real_clients,
    run_coordinated_gpio_test_lazy,
)
from kilnctrl.link_hub import get_shared_link
from kilnctrl.safety import SafetyClient
from kilnctrl.devices_safety import SafetyFlag
from kilnctrl.profiles import ProfilesClient
from kilnctrl import ota_http_client as ota_http
from kilnctrl import host_resolve


def _cli_preflight(link) -> GpioTestPreflight:
    """Same live checks the MCP tool runs -- see
    `mcp_server_coordinated_gpio_test.py`'s `_gpio_test_preflight()` for the
    HTTP/link details this mirrors for a standalone script that has no MCP
    server plumbing to reuse."""
    safety = SafetyClient(link)
    profiles = ProfilesClient(link)
    try:
        status = safety.get_status()
        safety_armed = bool(status.flags & SafetyFlag.ENABLED)
        link_up = bool(status.flags & SafetyFlag.LINK_UP)
    except Exception:  # noqa: BLE001 -- any failure to read is "unknown", refuse
        safety_armed = None
        link_up = None
    try:
        exec_status = profiles.get_exec_status()
        running_or_paused = exec_status.state in (1, 2)
        state_name = exec_status.state_name
    except Exception:  # noqa: BLE001
        running_or_paused = None
        state_name = "(unknown)"
    host = host_resolve.resolve_default_host()
    try:
        interlock = ota_http.get_interlock(host)
        ota_ok = bool(interlock.get("ok"))
        ota_reason = str(interlock.get("reason") or "ok")
    except Exception as exc:  # noqa: BLE001
        ota_ok = None
        ota_reason = f"could not read /api/ota/interlock: {exc}"
    return GpioTestPreflight(
        safety_armed=safety_armed,
        profile_running_or_paused=running_or_paused,
        profile_state_name=state_name,
        ota_interlock_ok=ota_ok,
        ota_interlock_reason=ota_reason,
        link_up=link_up,
    )


def main() -> int:
    print("=== Coordinated two-board GPIO test (HARDWARE.md section 1, Steps A/B) ===")
    print("Reminder: SWD to the Pico bonds GND_Safty to PC ground for the duration.")
    print("Bench only, no load wiring. Both boards must be powered.\n")

    link = get_shared_link()
    preflight_fn = lambda: _cli_preflight(link)
    # See mcp_server_coordinated_gpio_test.py's matching comment: preflight
    # is checked before the real (side-effectful) ESP client is ever built.
    build = lambda: build_real_clients(link, debug_probe, probe, get_preflight=preflight_fn)
    result = run_coordinated_gpio_test_lazy(preflight_fn, build, confirm=True)
    print(result.describe())
    return 0 if not result.refused and result.all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
