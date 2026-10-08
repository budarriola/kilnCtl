"""AUTOTUNE tools (task 10).

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
# AUTOTUNE (task 10) -- PID autotune, mirrors dashboard_http.c's /api/autotune*
# ---------------------------------------------------------------------------
@_core._tool()
def autotune_get_status() -> str:
    """Read the autotune run's current state, model fit and proposed gains."""
    try:
        st = _srv._autotune.get_status()
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    return (
        f"state={st.state_name} method={st.method} zone={st.zone} "
        f"elapsed={st.elapsed_s}s samples={st.sample_count} "
        f"actual={st.actual_c:.1f}C{'(valid)' if st.actual_valid else '(invalid)'} "
        f"duty={st.duty:.2f} model_valid={st.model_valid} model_settled={st.model_settled} "
        f"proposed_gains={st.proposed_gains} relay_valid={st.relay_valid}"
        # Only worth flagging once there is a real STEP-method result to
        # judge (state 5 = done, method 0 = step -- see AutotuneStatus.
        # STATE_NAMES/method docs); model_settled reads False by default
        # for every other state/method too and would otherwise print this
        # warning during an ordinary relay run or before a step test has
        # even produced a result.
        + (" -- NOT genuinely settled (ended via the 4h max-duration backstop); "
           "autotune_accept() will refuse this fit unless ack_unsettled=True is passed explicitly"
           if st.state == 5 and st.method == 0 and st.model_valid and not st.model_settled else "")
        + (f" abort_reason={st.abort_reason!r}" if st.abort_reason else "")
    )


@_core._tool()
def autotune_start(
    zone: int,
    method: str,
    step_duty_or_setpoint_c: float,
    relay_d: float = -1.0,
    relay_h_c: float = -1.0,
    rule: str = "",
) -> str:
    """Start autotune on a zone.

    ``method`` is "step" or "relay". ``rule`` selects the tuning rule from
    the full set the firmware defines (pid_autotune.h's autotune_rule_t,
    mirrored in devices.AUTOTUNE_RULES -- the single source of truth this
    tool and devices.autotune_start() both key off): "tl" (Tyreus-Luyben) or
    "zn" (Ziegler-Nichols) on the relay method, "simc" (the default) or
    "cohen-coon" on the step method. For the step method,
    ``step_duty_or_setpoint_c`` is the duty step (0..1); for the relay method
    it is the target setpoint in degC, and ``relay_d``/``relay_h_c`` (duty
    amplitude / hysteresis band) must also be given.

    ``rule`` left empty (the default) resolves per method -- "simc" for
    step, "tl" for relay -- so the most common call, autotune_start(zone,
    "step", duty), keeps working without the caller having to know a rule
    name at all. This mirrors dashboard_http.c's own defaulting (SIMC is
    the step path's default, Tyreus-Luyben the relay path's -- see
    autotune_start_post_handler()'s comments ~line 2021-2054).

    Note: "cohen-coon" is a real firmware rule (dashboard_http.c's HTTP
    POST /api/autotune/start accepts it on the step path) but the
    UART_TASK_ID_AUTOTUNE wire protocol this tool actually talks over has no
    rule byte for the step method yet (uart_bridge_ext.c always runs SIMC on
    that path, see devices.AUTOTUNE_RULES' wire_byte=None docstring) -- so
    it is refused here with an explicit error rather than silently starting
    a SIMC run while telling the caller Cohen-Coon was selected.
    """
    method_map = {"step": devices.AUTOTUNE_METHOD_STEP, "relay": devices.AUTOTUNE_METHOD_RELAY}
    #: dashboard_http.c's own per-method defaults (see the docstring above) --
    #: keeps the common no-rule-given call working for either method instead
    #: of forcing every step caller to know that "tl" (a relay-only rule) is
    #: not it.
    _DEFAULT_RULE_BY_METHOD = {"step": "simc", "relay": "tl"}
    method_key = method.strip().lower()
    rule_key = rule.strip().lower() or _DEFAULT_RULE_BY_METHOD.get(method_key, "")
    method_val = method_map.get(method_key)
    if method_val is None:
        return f"error: method must be one of {sorted(method_map)}, got {method!r}"
    rule_info = devices.AUTOTUNE_RULES.get(rule_key)
    if rule_info is None:
        return f"error: rule must be one of {sorted(devices.AUTOTUNE_RULES)}, got {rule_key!r}"
    if method_key not in rule_info["methods"]:
        return (f"error: rule {rule_key!r} is not valid for method {method!r} -- it applies to "
                f"{sorted(rule_info['methods'])} only (see devices.AUTOTUNE_RULES)")
    wire_byte = rule_info["wire_byte"]
    if wire_byte is None:
        return (f"error: rule {rule_key!r} is defined by the firmware but has no encoding on the "
                "UART_TASK_ID_AUTOTUNE wire protocol this tool uses (uart_bridge_ext.c's "
                "AUTOTUNE_CMD_START handler always runs SIMC for method=step) -- refusing to "
                "silently start a different rule than requested; use the board's HTTP "
                "POST /api/autotune/start if this rule is needed")
    # "implicit" (simc on the step method): the wire protocol has no rule
    # byte for this method at all -- the firmware runs SIMC regardless of
    # what is sent -- so any placeholder value round-trips correctly; 0 is
    # as good as any other.
    rule_val = 0 if wire_byte == "implicit" else wire_byte
    try:
        ok, err = _srv._autotune.start(zone, method_val, step_duty_or_setpoint_c, relay_d, relay_h_c, rule_val)
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    return "ok - autotune started" if ok else f"refused: {err}"


@_core._tool()
def autotune_abort() -> str:
    """Abort the running autotune."""
    try:
        result = _srv._autotune.abort()
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - aborted"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing running to abort{detail}"


@_core._tool()
def autotune_accept(ack_unsettled: bool = False) -> str:
    """Accept the finished autotune's proposed gains, writing them into the zone's PID config.

    Call autotune_get_status() first and check model_settled. A STEP result
    that never genuinely settled (it ended via the 4h max-duration backstop,
    not a real steady-state read) is refused unless ack_unsettled=True is
    passed explicitly here -- that is a deliberate choice to persist a
    lower-confidence fit, not this tool's default, so pass it only after
    actually looking at model_settled and the fitted K/tau/L and deciding
    they are still worth keeping (e.g. re-running the step test is usually
    the better option).
    """
    try:
        result = _srv._autotune.accept(ack_unsettled=ack_unsettled)
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - gains accepted" + (" (low-confidence: fit never genuinely settled)" if ack_unsettled else "")
    detail = f": {result.reason}" if result.reason else ""
    if result.reason and "ack_unsettled" not in result.reason and "no completed" not in result.reason:
        # System mode gate (owner decision 2026-10-08): accept is refused (HTTP 409) while a
        # firing or autotune run is active; say so rather than blaming the fit.
        return f"refused by system mode gate (409): {result.reason}"
    return f"refused - nothing to accept, or the fit never settled (see model_settled) and needs ack_unsettled=True{detail}"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
