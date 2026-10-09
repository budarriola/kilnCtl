"""TOUCH tools (task 13).

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
# TOUCH -- NS2009 touch controller on the same J2 panel (task 13)
#
# touch_inject is what lets this side "send touches as if from the screen":
# the firmware's screen_idle state machine treats it exactly like a real
# NS2009 press, resetting the auto-blank idle timer and waking the panel if
# it's currently blanked. touch_get_state is the query that shows the effect.
# ---------------------------------------------------------------------------
@_core._tool()
def touch_get_state() -> str:
    """Whether the panel is on right now, and how long it's been idle.

    A query answered from the firmware's in-memory screen_idle state, not a
    touch-controller round trip -- fast even if no NS2009 ever came up.
    """
    try:
        state = _srv._touch.get_state()
    except TouchQueryError as exc:
        return f"error: {exc}"
    return state.describe()


@_core._tool()
def touch_inject(x: int, y: int, pressed: bool = True) -> str:
    """Send a synthetic touch at (x, y), as if the physical panel were pressed.

    x/y are SCREEN PIXEL coordinates (0,0 at the top-left, same space every
    ui_page_*.c file lays widgets out in) -- the firmware applies them
    directly to LVGL's input device, downstream of the NS2009 calibration
    transform, so this call hit-tests real buttons/containers exactly like a
    finger would, independent of whether this board has ever been
    touch-calibrated. It also resets the firmware's screen auto-blank idle
    timer and wakes the panel if it is currently blanked, exactly as a real
    touch would.

    pressed=True latches the touch: the firmware keeps reporting it at (x, y)
    on every LVGL input poll until a matching pressed=False call releases it
    (one press + one release == one click, same as a real tap), or until
    ~5s pass with no follow-up call, at which point the firmware auto-releases
    it on its own so a forgotten release can't wedge the UI. To drag/scroll,
    send one pressed=True call, then further pressed=True calls with updated
    (x, y) tracing the path, then a final pressed=False to release -- each
    call is one point along the gesture, there is no separate "move" verb.
    An injected press takes priority over the physical NS2009 for as long as
    it is held, so it will not race a stray touch on the bench.
    """
    from .mcp_server_io import _touch_mutating  # local import: avoids a circular import with mcp_server_io.py

    return _touch_mutating(f"touch injected ({x},{y},{'down' if pressed else 'up'})",
                            lambda: _srv._touch.inject(x, y, pressed))


@_core._tool()
def touch_set_tap_dump(enable: bool) -> str:
    """Turn the firmware's AUTOMATIC per-page-switch tap-target dump on/off.

    Off by default: kiln_ui_show() dumps every clickable widget's rectangle,
    centre point, and label on every page switch only while this is enabled,
    because doing it unconditionally floods the device _srv.log during ordinary
    navigation. touch_log_tap_targets() below is unaffected by this flag and
    always dumps on request -- reach for that first; only turn this on when
    driving a sequence of navigations and wanting every switch's targets
    logged without a separate call after each one.
    """
    from .mcp_server_io import _touch_mutating  # local import: avoids a circular import with mcp_server_io.py

    return _touch_mutating(
        f"tap dump {'on' if enable else 'off'}", lambda: _srv._touch.set_tap_dump(enable)
    )


@_core._tool()
def touch_log_tap_targets() -> str:
    """Request an immediate tap-target dump for whatever screen is loaded now.

    This is the only way to discover where a widget actually is on this
    panel -- there is no framebuffer readback. The dump covers the active
    screen plus lv_layer_top()/lv_layer_sys() (where modal overlays such as
    ui_confirm.c's confirmation dialogs and ui_num_pad.c's keypad live), and
    walks any open lv_keyboard's individual keys. The dump itself still
    arrives as ESP_LOGI "tap target ..." lines over the device _srv.log
    (get_device_log / get_device_log_json), not as this call's return value --
    but a driver-error refusal is no longer silent, see _touch_mutating().
    """
    from .mcp_server_io import _touch_mutating  # local import: avoids a circular import with mcp_server_io.py

    return _touch_mutating("tap-target dump requested", lambda: _srv._touch.log_tap_targets())

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
