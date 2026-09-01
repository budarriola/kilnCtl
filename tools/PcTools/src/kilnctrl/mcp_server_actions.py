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

from . import mcp_server as _srv


# ---------------------------------------------------------------------------
# generic button press (actions.py)
#
# Every tool above is also reachable through here by name -- this exists so
# an agent doesn't need a bespoke tool per GUI button/menu-item, and so a
# future GUI button automatically gets MCP coverage the moment it's added to
# actions.py, without a matching @_srv._tool() having to be hand-written too.
# ---------------------------------------------------------------------------
@_srv._tool()
def list_buttons() -> str:
    """List every button/action press_button can invoke, with its parameters.

    Names match the GUI's own button/menu-item labels 1:1 (e.g. "Thermo: Read
    All", "IO: Set Relay"), grouped by popup/menu.
    """
    lines = []
    for name in sorted(actions.ACTIONS):
        action = actions.ACTIONS[name]
        params = ", ".join(f"{p}: {t.__name__}" for p, t in action.params.items())
        lines.append(f"{name}({params})\n    {action.description}")
    return "\n".join(lines)


@_srv._tool()
def press_button(name: str, params: Optional[dict[str, Any]] = None) -> str:
    """Press any GUI button/menu-item by name -- call list_buttons() first.

    `params` is a JSON object matching that action's parameter names, e.g.
    press_button("IO: Set Relay", {"relay": 1, "on": true})
    or press_button("Thermo: Read All") for an action that takes none.
    """
    action = actions.ACTIONS.get(name)
    if action is None:
        return f"error: unknown action {name!r}. Call list_buttons() for the full list."
    if params is not None and not isinstance(params, dict):
        return f"error: params must be a JSON object of named arguments, got {type(params).__name__}"
    try:
        return action.run(_srv._action_ctx, **(params or {}))
    except TypeError as exc:
        return f"error: bad arguments for {name!r}: {exc}"
    except ValueError as exc:
        return f"error: {exc}"
    except Exception as exc:  # noqa: BLE001 - an action must never raise into the transport
        _srv.log.exception("unexpected error in action %r", name)
        return f"error: unexpected {type(exc).__name__} in {name!r}: {exc}"


