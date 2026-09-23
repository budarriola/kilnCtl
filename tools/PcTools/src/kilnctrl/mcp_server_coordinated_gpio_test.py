"""Coordinated two-board GPIO test MCP tool -- exposes
`coordinated_gpio_test.py` through the sanctioned tool facade, same "own
module, plumbed through mcp_server.py's star-import list" pattern
mcp_server_capability_preflight.py/mcp_server_ramp_assist.py use.

Before this module, `tools/PcTools/scripts/coordinated_gpio_test.py`
implemented HARDWARE.md section 1 Steps A/B with no MCP registration and no
precondition checks at all -- a bench agent could not run it through the
facade, and a bare invocation would happily detach GPIO4/5/10 from both
firmwares' UART peripherals regardless of whether the safety relay was
armed, a firing was running, an OTA was mid-flight, or the link had ever
been confirmed up. See `coordinated_gpio_test.py`'s module docstring for the
full precondition/deny-list rationale this tool enforces before it drives a
single pin."""
from __future__ import annotations

from typing import Optional

from . import debug_probe, host_resolve, ota_http_client as ota_http, probe
from .coordinated_gpio_test import (
    GpioTestPreflight,
    build_real_clients,
    run_coordinated_gpio_test_lazy,
)
from .devices_safety import SafetyFlag
from .link_hub import get_shared_link
from .wifi_uart import WifiUartQueryError

from . import mcp_server as _srv


def _gpio_test_resolve_host(host: Optional[str]) -> str:
    """Same resolution order as every other HTTP-backed tool in this
    package: explicit ``host`` wins, else the board's current station IP,
    else the fallback-AP address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return host_resolve.resolve_default_host()


def _gpio_test_preflight(host: Optional[str]) -> GpioTestPreflight:
    """Read the four live preconditions this tool refuses on: safety not
    ARMED and the link already confirmed up (both from `_srv._safety`'s
    cached GET_STATUS, the same client `safety_get_status` uses), no
    profile running/paused (`_srv._profiles.get_exec_status()`, the same
    client `profiles_get_exec_status` uses), and the OTA interlock reporting
    idle (`ota_http_client.get_interlock`, GET `/api/ota/interlock`,
    ROUTE_TIER_ADMIN per `route_tier_table.h`, the same route
    `capability_preflight_check` reads). Any read failure is folded into `None` ("could not determine"),
    which `GpioTestPreflight.refusal_reasons()` never treats as satisfied."""
    try:
        status = _srv._safety.get_status()
        safety_armed = bool(status.flags & SafetyFlag.ENABLED)
        link_up = bool(status.flags & SafetyFlag.LINK_UP)
    except Exception:  # noqa: BLE001 -- "could not check" must refuse, not pass
        safety_armed = None
        link_up = None
    try:
        exec_status = _srv._profiles.get_exec_status()
        running_or_paused = exec_status.state in (1, 2)
        state_name = exec_status.state_name
    except Exception:  # noqa: BLE001
        running_or_paused = None
        state_name = "(unknown)"
    resolved_host = _gpio_test_resolve_host(host)
    try:
        interlock = ota_http.get_interlock(resolved_host)
        ota_ok = bool(interlock.get("ok"))
        ota_reason = str(interlock.get("reason") or "ok")
    except Exception as exc:  # noqa: BLE001
        ota_ok = None
        ota_reason = f"could not read /api/ota/interlock at {resolved_host}: {exc}"
    return GpioTestPreflight(
        safety_armed=safety_armed,
        profile_running_or_paused=running_or_paused,
        profile_state_name=state_name,
        ota_interlock_ok=ota_ok,
        ota_interlock_reason=ota_reason,
        link_up=link_up,
    )


@_srv._tool()
def coordinated_gpio_test(confirm: bool = False, host: Optional[str] = None) -> str:
    """Run the coordinated two-board GPIO test -- `firmware/SaftyFW/docs/
    HARDWARE.md` section 1, Steps A and B -- proving the isolated safety-link
    UART wiring (ESP GPIO4/GPIO5 <-> Pico GPIO4/GPIO5) by direct pin control,
    independent of both firmwares' own UART peripherals. This is the only
    check in the project that proves the crossing rather than assuming it
    from the schematic.

    DESTRUCTIVE-ADJACENT AND BENCH-ONLY: for its duration this halts the
    Pico and detaches GPIO4/5/10 from both firmwares' UART peripherals, then
    restores both boards (reset to run mode) afterward regardless of outcome.
    Both boards must already be powered and connected (ESP over its normal
    USB-serial link, Pico over its SWD debug probe) -- SWD to the Pico bonds
    GND_Safty to PC ground for the duration.

    REFUSES BEFORE TOUCHING ANY PIN unless every one of these holds:
      - the safety relay is confirmed NOT ARMED (SafetyFlag.ENABLED clear on
        the ESP's cached GET_STATUS)
      - no profile is running or paused (`profiles_get_exec_status`)
      - the OTA interlock reports idle (`GET /api/ota/interlock`)
      - the safety link was already confirmed up (SafetyFlag.LINK_UP) --
        this test can only tell you the raw wiring is bad by starting from a
        link already known good, since it necessarily interrupts that link
        itself
      - `confirm=True` is passed
    A precondition that could not be read at all (an exception, or the
    underlying client returning "unknown") is treated as a refusal, never as
    "assume it's fine". Also NEVER drives or reads GPIO6 (the ESP's Fault
    line output, already deny-listed in `gpio_probe.c` -- and, on the Pico,
    `SAFTYFW_PIN_RELAY`, the safety relay/heat-enable drive to Q4/K4) or
    GPIO9 (the E-stop input) on either side, regardless of caller -- enforced
    a second time, in code,
    immediately before every pin access (`coordinated_gpio_test.guard_pin`),
    as defense in depth on top of the preconditions above.

    Returns a per-step PASS/FAIL report (expected vs. observed level for
    each of the four drive/read measurements across Steps A and B) or, if
    refused, the specific reason(s) -- never a bare error.

    `host`: board IP/hostname for the `/api/ota/interlock` probe. Same
    resolution as every other HTTP-backed tool here: explicit host wins,
    else the board's current station IP, else the fallback-AP address.
    """
    if not confirm:
        return (
            "REFUSED -- confirm=True was not passed -- refusing to drive any pin.\n"
            "This test halts the Pico and detaches GPIO4/5/10 from both firmwares' "
            "UART peripherals for its duration; call again with confirm=True once "
            "you have confirmed both boards are powered and connected."
        )
    link = get_shared_link()
    preflight_fn = lambda: _gpio_test_preflight(host)
    # build_real_clients() constructs the real ESP ProbeClient eagerly, which
    # registers a task and starts a background thread on the shared link --
    # run_coordinated_gpio_test_lazy checks preflight/confirm FIRST and only
    # calls this builder once it's known the run will proceed, so a refusal
    # never leaks a client (Opus review of bdd06947).
    build = lambda: build_real_clients(link, debug_probe, probe, get_preflight=preflight_fn)
    result = run_coordinated_gpio_test_lazy(preflight_fn, build, confirm=confirm)
    return result.describe()
