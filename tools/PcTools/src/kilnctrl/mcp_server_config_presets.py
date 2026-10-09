"""CONFIG PRESETS.

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
# CONFIG PRESETS -- known-good starting configs for a consistent test basis.
# Data-driven (tools/PcTools/config_presets/*.json), never compiled into
# firmware -- see config_presets.py's module docstring for the full SCOPE
# rationale (only PID/model are written; relay_mask/max_temp_c/control_mode
# are read-only over this link today, hook documented there).
# ---------------------------------------------------------------------------
@_core._tool()
def list_config_presets() -> str:
    """List every known-good config preset (name + description).

    Presets live as JSON under ``tools/PcTools/config_presets/`` -- never
    compiled into firmware, so a bench-only setting (like a fixture's
    lowered temperature ceiling) can never leak into a real kiln build.
    """
    presets = config_presets.list_presets()
    if not presets:
        return f"no presets found under {config_presets.presets_dir()}"
    return "\n".join(f"{p['name']}: {p['description']}" for p in presets)


@_core._tool()
def load_config_preset(name: str, host: Optional[str] = None,
                        safety_host: Optional[str] = None,
                        use_ct_map_backup: bool = False,
                        confirm: bool = False) -> str:
    """Apply a known-good preset's zone config to the live board.

    PID gains and (when the preset carries one) the thermal model go over
    the UART CONTROL task, same as always. `host` (new): when given, ALSO
    writes relay_mask/control_mode/max_temp_c/min_temp_c/max_ramp_c_per_hr/
    thermo_count/relay_count over GET/POST /api/zones -- the fields the UART
    link has no setter for (zones_http_client.py). That write GETs the live
    config first and merges the preset onto it (POST /api/zones is a
    whole-page-submit endpoint: a naive partial POST would zero every field
    it doesn't mention, including max_temp_c -- see that module's docstring
    for why this matters with heating elements physically connected), then
    re-reads the config and confirms it actually landed -- a preset that
    reports success without confirming is the exact defect class this repo
    has been fighting elsewhere. Host resolution matches the OTA tools: see
    `_ota_resolve_host()`.

    `host` omitted (the default): those fields are reported as
    reference/expected data only, NOT written -- unchanged from before this
    parameter existed.

    `safety_host` (new): when given, ALSO writes the preset's "safety"
    section -- the SAFETY PROCESSOR's commissioning parameters (tc_type,
    abs_max_temp_c, mains_voltage_v, ...) -- over
    POST /api/safety/commissioning, staged as SET_PARAM and committed with
    COMMIT_CONFIG, then re-read and confirmed field by field. This is the
    SAME ESP as `host` in practice: both endpoints are served by the ESP32,
    which is the only thing that can talk to the Pico. Pass the same address
    to both. Omitted: the preset's safety values are reported as
    reference/expected data only, not written.

    `use_ct_map_backup` (default False): also writes the preset's
    "safety_ct_channel_map_backup" section -- an ASSUMED, UNMEASURED
    CT-to-zone map. Committing it clears calibration_missing, which is what
    lets the safety processor grant heat, so a bench whose CTs are not
    fitted would start reporting itself COMMISSIONED on a mapping nobody
    measured. Only pass this when you specifically want that (a host-test
    scenario, or a deliberate exercise of the commissioning gate's accept
    path) and know the map is fabricated.

    Does NOT reset, does NOT touch relays, does NOT request enable.

    Refuses unless ``confirm is True`` exactly (whole-page writes of PID
    gains, zones and optionally safety fields), and refuses while a profile
    firing or autotune run is live or cannot be ruled out.
    """
    if confirm is not True:
        return (f"refused: load_config_preset({name!r}) overwrites PID gains for every zone "
                "(and zones/safety config when host/safety_host is given); pass "
                "confirm=True, exactly, to apply.")
    from .mcp_server_control import _profile_or_autotune_running_reason  # local: avoids an import cycle
    running = _profile_or_autotune_running_reason()
    if running is not None:
        return f"refused: {running} -- will not apply a config preset mid-run"
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    try:
        preset = config_presets.load_preset_data(name)
    except config_presets.ConfigPresetError as exc:
        return f"error: {exc}"
    resolved = _ota_resolve_host(host) if host else None
    resolved_safety = _ota_resolve_host(safety_host) if safety_host else None
    try:
        result = config_presets.apply_preset(
            _srv._control, preset, zones_host=resolved, safety_host=resolved_safety,
            use_ct_map_backup=use_ct_map_backup)
    except ControlQueryError as exc:
        return f"error: {exc}"
    except zones_http_client.ZonesHttpError as exc:
        return f"error writing zones config over HTTP (host={resolved}): {exc}"
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return f"error writing safety config over HTTP (host={resolved_safety}): {exc}"
    return result.describe()

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
