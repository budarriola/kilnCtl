"""UI regression-test scripts.

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

from . import actions, config_presets, debug_probe, devices, factory_reset_guard, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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
# UI regression-test scripts -- LCD (task 14, UiTestClient) + web (HTTP
# dashboard, WebUiClient) halves of the same framework. Scripts are DATA
# under tools/PcTools/ui_scripts/*.json, same "never compiled into firmware"
# reasoning as config_presets.py.
# ---------------------------------------------------------------------------
@_core._tool()
def ui_list_scripts() -> str:
    """List every UI regression-test script (name + description + backend).

    Scripts live as JSON under ``tools/PcTools/ui_scripts/``.
    """
    scripts = ui_test_runner.list_ui_scripts()
    if not scripts:
        return f"no ui scripts found under {ui_test_runner.ui_scripts_dir()}"
    return "\n".join(f"{s['name']} ({s['backend']}): {s['description']}" for s in scripts)


@_core._tool()
def ui_run_script(name: str, zones_host: Optional[str] = None, apply_preset: bool = False,
                  confirm: bool = False) -> str:
    """Run one UI regression-test script end to end and report a compact
    pass/fail per step.

    ``zones_host``/``apply_preset``: same meaning as load_config_preset()'s
    -- forwarded to config_presets.apply_preset() when the script names a
    ``preset``. ``apply_preset`` defaults to False: applying a preset
    OVERWRITES every zone's PID gains and thermal model (and zones/ramp_assist
    with ``zones_host``). It therefore needs ``confirm=True`` exactly, refuses
    while a profile or autotune run is live or cannot be ruled out, and FAILS
    the call (no steps run) if any preset stage did not land. The web backend resolves its base_url the same way OTA does
    (see ``_ota_resolve_host``); the lcd backend drives the already-connected
    board over task 14.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    try:
        script = ui_test_runner.load_ui_script(name)
    except ui_test_runner.UiScriptError as exc:
        return f"error: {exc}"
    if apply_preset and "preset" in script:
        if confirm is not True:
            return (f"refused: ui_run_script({name!r}, apply_preset=True) overwrites PID gains and the thermal "
                    "model for every zone in the script's preset; pass confirm=True, exactly, to apply.")
        from .mcp_server_control import _profile_or_autotune_running_reason  # local: avoids an import cycle
        running = _profile_or_autotune_running_reason()
        if running is not None:
            return f"refused: {running} -- will not apply a script preset mid-run"
    web_client = None
    if script["backend"] == "web":
        web_client = WebUiClient(f"http://{_ota_resolve_host(zones_host)}")
    try:
        result = ui_test_runner.run_ui_script(
            name, ui_test_client=_srv._ui_test, web_client=web_client,
            zones_host=zones_host, apply_preset=apply_preset,
        )
    except (ui_test_runner.UiScriptError, config_presets.ConfigPresetError) as exc:
        return f"error: {exc}"
    return json.dumps(result)


@_core._tool()
def ui_step(backend: str, action: str, target: str, timeout_ms: int = 3000,
            contains: Optional[str] = None, value: Optional[str] = None) -> str:
    """Run a single UI step directly -- the debug entry point for trying one
    action without a whole script (see ui_test_runner.run_ui_step()).
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    step = {"action": action, "target": target, "timeout_ms": timeout_ms}
    if contains is not None:
        step["contains"] = contains
    if value is not None:
        step["value"] = value
    web_client = WebUiClient(f"http://{_ota_resolve_host(None)}") if backend == "web" else None
    try:
        result = ui_test_runner.run_ui_step(backend, _srv._ui_test, web_client, step)
    except ui_test_runner.UiScriptError as exc:
        return f"error: {exc}"
    return json.dumps(result)


#: FACTORY_RESET reboots ~500ms after the ACK; ESP32-S3 boot to first
#: GET_FW_VERSION push is normally a few seconds. Generous on purpose --
#: this only runs when a caller explicitly asked for a reboot.
FACTORY_RESET_REBOOT_TIMEOUT_S = 20.0


def _wiped_message(scope: int, backup: str, why: str) -> str:
    return (f"BOARD WIPED: factory reset (scope={scope}) HAPPENED and the follow-up failed -- {why}. "
            f"The board is at firmware defaults, NOT configured. Restore from the pre-reset "
            f"backup: {backup} (backup_import(path=..., confirm=True)), then verify "
            f"control_get_zones before any firing.")


@_core._tool()
def factory_default_then_load_preset(name: str, scope: int = FACTORY_RESET_SCOPE_KILN,
                                      host: Optional[str] = None,
                                      safety_host: Optional[str] = None,
                                      use_ct_map_backup: bool = False,
                                      confirm: bool = False,
                                      skip_backup: bool = False) -> str:
    """Factory-default the board, then apply a known-good preset -- one
    callable step so a test always starts from the same place.

    ``scope`` picks exactly what gets wiped (firmware/KilnFW/App/drivers/
    factory_reset.c's ``kScopes``, one ``nvs_flash_erase_partition()`` per
    partition named -- there is no blanket erase-everything path):
      * WIFI (0): erases ``wifi_nvs`` only -- Wi-Fi credentials/AP config.
        Zone config, saved profiles, relay cycle counters are untouched.
      * KILN (1, the default here -- matches this tool's own "consistent
        test basis" purpose): erases ``kiln_nvs`` only -- the WHOLE
        partition, wholesale, not just zones_cfg_t: zone config, relay
        names, rules, relay-cycle counters, and run_state all live there
        (see kiln.py 8.1's partition split) and all come back at their
        firmware defaults. Wi-Fi credentials and saved profiles are
        untouched, so the board stays reachable and the operator's fire
        schedules survive.
      * PROFILES (2): erases ``profiles_nvs`` AND explicitly restores every
        shipped built-in schedule to visible (``profiles_builtin_restore_
        all()``) -- "reset fire profiles" means both halves of what the
        /profiles page shows, not just clearing the user's 8 saved slots.
      * ALL (3): all three partitions, plus the PROFILES restore above.
        Drops Wi-Fi too -- the board falls back to its AP address
        (``ota_http.OTA_AP_DEFAULT_HOST``) until re-provisioned.
      In every case: FACTORY_RESET reboots the board ~500ms after the ACK
      (execute_scope()'s reboot_task) -- this call waits for the post-reboot
      firmware-version push before applying the preset, so it isn't racing
      the boot. See devices.FACTORY_RESET_SCOPE_* for the numeric values.

    `host` (new, same as load_config_preset()'s): when given, ALSO writes
    relay_mask/control_mode/max_temp_c/min_temp_c/max_ramp_c_per_hr/
    thermo_count/relay_count over GET/POST /api/zones after the reboot, with
    the same GET-merge-POST-then-verify contract load_config_preset()
    documents. Omitted (the default): those fields are reported as
    reference data only, matching this tool's pre-existing behavior. The
    reboot the KILN/ALL scopes trigger drops the board off any Wi-Fi network
    it was on only if scope includes wifi -- host resolution below still
    runs `_ota_resolve_host()` after the reboot, at whatever address is
    live then.

    `safety_host`/`use_ct_map_backup` (new): same meaning as
    load_config_preset()'s -- the preset's safety-processor section, written
    and read-back-verified over POST /api/safety/commissioning after the
    reboot. Note the factory-reset scopes above erase ESP NVS partitions
    only; the Pico's own config record is NOT wiped by any of them, so a
    safety section is a re-commit of the same values rather than a restore
    from blank.

    This is the disruptive lever in this module: it reboots the board and
    then writes PID/model gains (and, with `host`, zones config too). Never
    invoke it against a bench with a firing in progress or with the safety
    processor ARMED. Refuses unless ``confirm is True`` exactly (it erases
    NVS partitions).

    BACKUP FIRST (docs/audits/KILN_NVS_LOSS_2026-10-09.md): before sending the
    reset this exports GET /api/backup/export to logs/backup_export/ and
    REFUSES to reset if the export fails, unless ``skip_backup=True``. The
    saved path is reported in every result. If the preset apply fails after the
    reset the result says the board is WIPED and names the file to restore with
    ``backup_import``. Log every reset (time, reason) in docs/BENCH_TEST_LOG.md.
    """
    if confirm is not True:
        return (
            "error: refusing to factory-default the board without confirm=True -- "
            f"scope {scope} erases NVS partition(s) and reboots the board. Pass "
            "confirm=True once you have reviewed exactly what this will destroy."
        )
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py
    from .mcp_server_control import _profile_or_autotune_running_reason  # local: avoids an import cycle

    running = _profile_or_autotune_running_reason()
    if running is not None:
        return f"refused: {running} -- will not factory-default the board mid-run (nothing erased)"
    try:
        preset = config_presets.load_preset_data(name)
    except config_presets.ConfigPresetError as exc:
        return f"error: {exc}"
    ok, backup = factory_reset_guard.backup_before_reset(_ota_resolve_host(host), skip_backup)
    if not ok:
        return backup
    # Arm BEFORE sending, so a reboot fast enough to push before the wait
    # below starts is still counted rather than cleared away.
    _srv._info.arm_boot_push()
    send_result = _srv._link.send(
        dst_task=UART_TASK_ID_SYSTEM, src_task=UART_TASK_ID_SYSTEM,
        payload=devices.system_factory_reset(scope), dst_device=Device.ESP,
    )
    if not send_result.ok:
        return f"error: factory reset request was not ACKed ({send_result})"
    # Wait for the device's own UNSOLICITED boot-time FW-version push, not a
    # polled get_fw_version() query -- a query is answered by the always-alive
    # INFO task whether or not a reboot ever happened, so it cannot tell a
    # real reboot apart from a factory reset the board's system mode gate
    # silently refused (execute_scope() is never reached, so no reboot
    # follows; see firmware/KilnFW/App/drivers/http/factory_reset.c's
    # FACTORY_RESET_ERR_MODE_GATE_REFUSED path and its UART counterpart in
    # uart_bridge_system.c). A refused reset still ACKs the request above
    # (the ACK only confirms delivery of the command, not that it will be
    # honored), so send_result.ok alone is not proof either.
    if _srv._info.wait_for_boot_push(timeout=FACTORY_RESET_REBOOT_TIMEOUT_S, arm=False) is None:
        return (
            f"refused: factory reset request was ACKed, but no reboot was observed "
            f"within {FACTORY_RESET_REBOOT_TIMEOUT_S}s (no unsolicited FW-version "
            f"boot push) -- most likely refused by the board's system mode gate "
            f"(e.g. a firing or autotune run in progress); preset NOT applied. "
            f"backup: {backup}"
        )
    resolved = _ota_resolve_host(host) if host else None
    resolved_safety = _ota_resolve_host(safety_host) if safety_host else None
    try:
        result = config_presets.apply_preset(
            _srv._control, preset, zones_host=resolved, safety_host=resolved_safety,
            use_ct_map_backup=use_ct_map_backup)
    except ControlQueryError as exc:
        return _wiped_message(scope, backup, f"preset apply failed: {exc}{_partial_note(exc)}")
    except zones_http_client.ZonesHttpError as exc:
        return _wiped_message(scope, backup, f"PID/model applied, but zones config write failed (host={resolved}): {exc}{_partial_note(exc)}")
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return _wiped_message(scope, backup, "PID/model applied, but safety config write failed "
                              f"(host={resolved_safety}): {exc}{_partial_note(exc)}")
    except Exception as exc:  # noqa: BLE001 -- after a wipe EVERY failure must still name the backup
        return _wiped_message(scope, backup, f"preset apply raised {type(exc).__name__}: {exc}{_partial_note(exc)}")
    if not result.all_ok:
        return _wiped_message(scope, backup, "preset only PARTIALLY applied:\n" + result.describe())
    return f"factory reset ok (scope={scope})\nbackup: {backup}\n" + result.describe()


def _partial_note(exc: BaseException) -> str:
    """Stages that landed before ``exc`` (config_presets.apply_preset attaches
    ``preset_partial``), or empty."""
    partial = getattr(exc, "preset_partial", None)
    return f"\nstages completed before the failure:\n{partial}" if partial else ""


# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
