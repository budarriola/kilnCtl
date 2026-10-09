"""generic button press (actions.py).

Part of the mcp_server.py split (pure refactor) -- moved verbatim, no
logic changes. See mcp_server.py's module docstring for the overall map.
"""
from __future__ import annotations

import asyncio
import dataclasses
import functools
import glob
import json
import math
import logging
import os
import subprocess
import sys
import threading
import time
from collections import deque
from typing import Any, Callable, Optional

from . import actions, config_presets, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
from .autotune import AutotuneClient, AutotuneQueryError
from .control import ControlClient, ControlQueryError
from .device_log import LogClient
from .devices import LogLine
from .display import BlitError, DisplayClient, DisplayQueryError
from .touch import TouchClient, TouchQueryError
from .ui_test_client import UiTestClient, UiTestQueryError
from .web_ui_client import WebUiClient
from .info import InfoClient, InfoQueryError
from .system import SystemClient, SystemQueryError
from .io_expander import IoClient, IoQueryError
from .link_hub import get_shared_link
from .profiles import ProfilesClient, ProfilesQueryError
from .protocol import (
    FACTORY_RESET_SCOPE_KILN,
    PROFILES_SAVE_ID_NEW,
    profile_id_is_builtin,
    THERMO_CHANNEL_ALL,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    WIFI_MODE_AP,
    WIFI_MODE_HOME,
    Device,
    Frame,
    FrameError,
    LogLevel,
    MsgType,
    unstuff,
)
from . import ota_http_client as ota_http
from . import probe
from .probe import ProbeClient, ProbeQueryError
from .wifi_uart import WifiUartClient, WifiUartQueryError
from .safety import SafetyClient, SafetyQueryError
from .serial_link import list_ports, recommend_port
from .session_log import SessionLogger
from .thermo import ThermoClient, ThermoQueryError

from . import mcp_server_core as _core


# ---------------------------------------------------------------------------
# generic button press (actions.py)
#
# Every tool above is also reachable through here by name -- this exists so
# an agent doesn't need a bespoke tool per GUI button/menu-item, and so a
# future GUI button automatically gets MCP coverage the moment it's added to
# actions.py, without a matching @_core._tool() having to be hand-written too.
# ---------------------------------------------------------------------------
@_core._tool()
def list_buttons() -> str:
    """List every button/action press_button can invoke, with its parameters.

    Names match the GUI's own button/menu-item labels 1:1 (e.g. "Thermo: Read
    All", "IO: Set Relay"), grouped by popup/menu. A parameter name suffixed
    with ``*`` is in that action's ``required`` set -- no implicit default,
    must be passed explicitly (e.g. "System: Factory Reset"'s ``scope*`` and
    ``confirm*``).
    """
    lines = []
    for name in sorted(actions.ACTIONS):
        action = actions.ACTIONS[name]
        params = ", ".join(
            f"{p}{'*' if p in action.required else ''}: {t.__name__}"
            for p, t in action.params.items()
        )
        lines.append(f"{name}({params})\n    {action.description}")
    return "\n".join(lines)


def _validate_param(param_name: str, action_name: str, value: Any, declared: type) -> Optional[str]:
    """press_button's type gate: a value must be exactly ``declared``, not
    merely coercible to it -- a JSON-transport quirk ("false"/"true", "0"/"1",
    or the integer 1) must never slip past a caller reading `if not confirm` /
    `confirm is True` in an action's own lambda (actions.py's factory-reset
    and watchdog-panic gates are exactly that shape). bool is checked before
    int since bool is an int subclass in Python (isinstance(True, int) is
    True) -- an int-typed param must reject True/False too, since silently
    reading those as 1/0 is the same class of bug.
    """
    if declared is bool:
        if not isinstance(value, bool):
            return (
                f"error: bad arguments for {action_name!r}: {param_name!r} must be a "
                f"real boolean (true/false), got {value!r} ({type(value).__name__})"
            )
    elif declared is int:
        if isinstance(value, bool) or not isinstance(value, int):
            return (
                f"error: bad arguments for {action_name!r}: {param_name!r} must be a "
                f"real integer, got {value!r} ({type(value).__name__})"
            )
    return None


@_core._tool()
def press_button(name: str, params: Optional[dict[str, Any]] = None) -> str:
    """Press any GUI button/menu-item by name -- call list_buttons() first.

    `params` is a JSON object matching that action's parameter names, e.g.
    press_button("IO: Set Relay", {"relay": 1, "on": true})
    or press_button("Thermo: Read All") for an action that takes none.

    Every supplied value declared bool or int is checked against that exact
    type before dispatch (see _validate_param) -- str/float-declared params
    are NOT checked here and pass through as-is, same as before this
    validation existed. The bool/int case is the one that mattered: a
    bool-typed param (e.g. a gated action's `confirm`) must be a real JSON
    boolean, not "false"/"true"/0/1 -- those used to pass straight through
    to the action's own `if not confirm` check, where a truthy string like
    "false" evaluates true and defeats the gate entirely. No gated action in
    this registry uses a str/float confirm-style param today; if one ever
    does, extend _validate_param first.
    """
    action = actions.ACTIONS.get(name)
    if action is None:
        return f"error: unknown action {name!r}. Call list_buttons() for the full list."
    if params is not None and not isinstance(params, dict):
        return f"error: params must be a JSON object of named arguments, got {type(params).__name__}"
    params = params or {}
    for param_name, value in params.items():
        declared = action.params.get(param_name)
        if declared is None:
            continue  # an unknown key still surfaces below as a TypeError from run()
        error = _validate_param(param_name, name, value, declared)
        if error is not None:
            return error
    try:
        return action.run(_srv._action_ctx, **params)
    except TypeError as exc:
        return f"error: bad arguments for {name!r}: {exc}"
    except ValueError as exc:
        return f"error: {exc}"
    except Exception as exc:  # noqa: BLE001 - an action must never raise into the transport
        _srv.log.exception("unexpected error in action %r", name)
        return f"error: unexpected {type(exc).__name__} in {name!r}: {exc}"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
