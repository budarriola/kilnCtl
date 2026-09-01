"""OTA over HTTP.

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
# OTA (firmware/CommonFW/docs/UPDATE_PROTOCOL.md, KilnFW/TODO.md 9.5/9.6) --
# HTTP, not the UART link. All four tools below drive ota_http.c's
# /api/ota/{challenge,esp,pico} + /api/ota/pico/status endpoints over the
# board's own web server -- request construction, HMAC signing, and response
# parsing live in ota_http_client.py (unit-tested there with mocked HTTP
# responses; see tools/PcTools/tests/test_ota_http_client.py). NEVER
# exercised against real hardware from here -- no board is attached in CI or
# in this pass's dev environment. Live-board verification (does a real ESP
# accept these bytes end to end, do the lockout/interlock paths behave as
# documented) is still outstanding.
#
# Host discovery mirrors gui.py's _wifi_default_host(): prefer the board's
# current station IP (from the UART-side WIFI tools, which always work even
# with Wi-Fi itself down or never provisioned), fall back to the board's own
# fallback-AP address. An explicit `host` argument always wins over both --
# useful for kilnctl.local (mDNS) or a host on a network the UART link can't see
# into (e.g. this MCP server's serial port is on a different PC than the one
# actually joined to the board's Wi-Fi).
#
# These are destructive-adjacent, safety-relevant operations (flashing a
# kiln controller and its safety processor). Nothing here retries a partial
# write on its own -- a failed push is left failed, and re-uploading is a
# separate, explicit tool call, never something a caller has to guess
# happened silently underneath these.
# ---------------------------------------------------------------------------
def _ota_resolve_host(host: Optional[str]) -> str:
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return ota_http.OTA_AP_DEFAULT_HOST


@_srv._tool()
def ota_get_challenge(host: Optional[str] = None) -> str:
    """GET /api/ota/challenge -- issue a fresh single-use OTA auth nonce.

    Mostly a diagnostic/manual tool: ota_update_esp()/ota_update_pico() below
    already fetch their own challenge internally, so this is not a required
    first step for a normal push. Useful to confirm the board's OTA HTTP
    surface is reachable at all, or to hand-verify the HMAC scheme.

    `host`: board IP or hostname (e.g. "192.168.1.42" or "kilnctl.local").
    Defaults to the board's current station IP (via wifi_get_status()'s UART
    query) if connected, else the board's fallback-AP address 192.168.4.1.
    """
    resolved = _ota_resolve_host(host)
    try:
        nonce = ota_http.get_challenge(resolved)
    except ota_http.OtaHttpError as exc:
        return f"error: {exc} (host={resolved})"
    return f"ok - nonce={nonce.hex()} host={resolved} (single-use, 30s expiry)"


@_srv._tool()
def ota_update_esp(image_path: str, password: str, host: Optional[str] = None) -> str:
    """Push a new ESP32-S3 firmware image over Wi-Fi -- POST /api/ota/esp.

    DESTRUCTIVE-ADJACENT: this streams `image_path` (a raw ESP-IDF .bin,
    e.g. KilnCtrl.bin) straight into the inactive OTA slot and sets it as
    the next boot partition on success. Refused by the board itself unless
    every interlock in UPDATE_PROTOCOL.md section 1 holds (kiln idle, no
    heater commanded, safety link healthy, temperature below the configured
    ceiling) -- a refusal comes back here as a specific error naming the
    unmet precondition, not a generic failure.

    `password`: the board's AP password (same one wifi_prov_get_ap_password()
    returns) -- used only to derive the challenge-response HMAC per
    CommonFW/docs/UPDATE_PROTOCOL.md section 2; the plaintext password is
    never sent over the wire.

    On success the image is written and set as the boot partition, but stays
    PENDING_VERIFY until the board reboots AND main.c's
    ota_rollback_confirm_task() confirms NVS/safety-link/web-server are all
    up post-reboot -- this call does not reboot the board itself, and does
    not wait for or confirm that verification. Nothing here retries a
    partial write: a failure mid-transfer is left failed on both sides
    (ota_esp_do_transfer()'s single cleanup path aborts the OTA handle and
    releases the update mutex), and re-uploading is a fresh, separate call.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment).
    """
    resolved = _ota_resolve_host(host)
    try:
        result = ota_http.push_esp_image(resolved, image_path, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not result.ok:
        return f"error: board reported failure: {result.body} (host={resolved})"
    b = result.body
    return (f"ok - wrote {b.get('bytes')} bytes to {b.get('partition')!r}, "
            f"new version={b.get('version')!r} (host={resolved}) -- "
            f"PENDING_VERIFY until the board reboots and self-confirms; "
            f"reboot required for the new image to run")


@_srv._tool()
def ota_rollback_esp(password: str, host: Optional[str] = None) -> str:
    """Explicitly revert the ESP32-S3 to its PREVIOUS firmware image, right
    now -- POST /api/ota/esp/rollback.

    This is the OTHER half of the rollback story: ota_rollback_confirm_task()
    (App/main.c) already handles the "don't auto-revert a healthy new image"
    side (it confirms the currently-running new image as good once NVS/
    safety-link/web-server are all up post-reboot). This tool is the
    "operator/agent wants to go back to the previous image on purpose" side
    -- even though the currently running image is healthy and already
    confirmed. Genuinely rare: ordinary recovery from a BAD update needs no
    call here at all, since an unconfirmed PENDING_VERIFY image is already
    reverted automatically by the bootloader on the next boot. Use this when
    the running image is valid but behaves worse in practice than the one it
    replaced, and a deliberate revert is wanted.

    DESTRUCTIVE-ADJACENT, same class as ota_update_esp(): refused unless the
    same interlocks in UPDATE_PROTOCOL.md section 1 hold (kiln idle, no
    heater commanded, safety link healthy, temperature below the configured
    ceiling) -- the board reboots into different code either way, so a
    rollback is exactly as disruptive as a push. ALSO refused, with a
    specific "no previous valid image to roll back to" reason, if there is
    genuinely no earlier valid image to fall back to
    (esp_ota_check_rollback_is_possible() on the board) -- this is checked
    before the reboot is attempted, not discovered by a blind call that
    fails partway.

    `password`: the board's AP password, same HMAC scheme ota_update_esp()
    uses -- but signed over a DIFFERENT context ("esp-rollback", not "esp"),
    so a MAC captured for one action cannot be reused to authorize the
    other. The plaintext password is never sent over the wire.

    On success, the board has already persisted an ota_record (processor
    "esp", success=true, reason "rollback requested") and is rebooting into
    the previous image from a short-lived background task -- this call
    returns as soon as the board's response arrives, it does NOT wait for
    the reboot to finish or re-check the version afterward. Call this
    tool's caller's own version check (e.g. get_fw_version, once the board
    is back up) to confirm which image is actually running now.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment); no ESP32-S3 was available in
    this environment to actually trigger a reboot/rollback.
    """
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.rollback_esp(resolved, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - rollback accepted, was running version={body.get('version_before')!r} "
            f"(host={resolved}) -- board is rebooting into the previous image now; "
            f"re-check the version once it comes back up")


@_srv._tool()
def ota_recovery_exit_esp(password: str, host: Optional[str] = None) -> str:
    """Ask the ESP32-S3 to reboot right now to exit boot_guard.h's recovery
    mode -- POST /api/ota/esp/recovery_exit.

    Recovery mode means a recent update did not self-confirm within its
    window; the board already self-clears the counter that put it there
    within OTA_CONFIRM_POLL_MS of any boot (recovery-mode boots included),
    so the NEXT reboot lands back in normal mode on its own. This tool is
    the impatient/uncertain case: reboot right now instead of waiting for
    that background self-clear (or a watchdog) to do it.

    `password`: same AP-password-derived HMAC scheme as ota_update_esp()/
    ota_rollback_esp() -- but signed over yet another distinct context
    ("recovery", not "esp" or "esp-rollback"), so a MAC captured for one
    action cannot be reused to authorize this one. Refused (403) on a wrong
    password/lockout, same as the other OTA routes, and ALSO refused (403,
    "board is not in recovery mode") if the board is not currently in
    recovery mode -- that check runs after auth specifically so a caller
    who never proves they hold the AP password cannot use this to probe
    whether the board is in recovery mode.

    On success, the board is already rebooting from a short-lived
    background task -- this call returns as soon as the response arrives,
    it does NOT wait for the reboot to finish.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only; no ESP32-S3 was
    available in this environment to actually trigger recovery mode and
    exit it.
    """
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.recovery_exit_esp(resolved, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - recovery-mode exit accepted (host={resolved}) -- "
            f"board is rebooting now")


@_srv._tool()
def ota_update_pico(image_path: str, password: str, host: Optional[str] = None) -> str:
    """Push a new RP2040 safety-processor firmware image -- POST
    /api/ota/pico. Stages `image_path` (a raw SaftyFW .bin) into the ESP's
    `pico_img` partition at Wi-Fi speed, then hands off to a background task
    that relays it to the RP2040 over the isolated UART link (~35s+,
    unacknowledged UPDATE_DATA broadcast + gap-report retransmit -- see
    CommonFW/docs/UPDATE_PROTOCOL.md section 4).

    DESTRUCTIVE-ADJACENT, and this is the SAFETY PROCESSOR: refused unless
    the same interlocks as ota_update_esp() hold, checked independently by
    BOTH processors (the Pico does not take the ESP's word for it). A
    refusal (wrong password, an interlock, or a concurrent update already
    in progress) comes back as a specific board-reported reason.

    `password`: same AP-password-derived HMAC scheme as ota_update_esp() --
    see that tool's doc comment.

    IMPORTANT: a successful response here means "the image was staged and
    the relay STARTED" -- it does NOT mean the RP2040 is now running the new
    image. The relay itself takes ~35 seconds or more and happens in the
    background after this call returns (202 Accepted). Call
    ota_status(host) afterward, repeatedly, to learn whether the relay
    actually completed, failed, or is still in progress. Nothing here
    retries a partial write on its own -- ota_pico_do_stage()'s single
    cleanup path releases the update mutex on any staging failure, and the
    relay task itself caps retransmission rounds and fails cleanly rather
    than looping forever on a bad link.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- neither the ESP HTTP path nor
    the RP2040 relay has been exercised against physical boards from this
    tool; only request construction/HMAC/response-parsing are unit-tested,
    with mocked HTTP.
    """
    resolved = _ota_resolve_host(host)
    try:
        result = ota_http.push_pico_image(resolved, image_path, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not result.ok:
        return f"error: board reported failure: {result.body} (host={resolved})"
    b = result.body
    return (f"ok - staged {b.get('bytes')} bytes, crc32={b.get('crc32')!r}, "
            f"relay started (host={resolved}) -- this does NOT mean the "
            f"RP2040 has finished updating; poll ota_status(host) for the "
            f"actual relay outcome")


@_srv._tool()
def ota_status(host: Optional[str] = None) -> str:
    """Poll OTA update progress -- both GET /api/ota/pico/status and
    GET /api/ota/esp/status.

    Reports the background Pico relay task's phase/percent/last_error
    (ota_pico_relay.c) -- e.g. "sending" at 60%, or "done"/"failed" once the
    relay has finished -- AND the ESP's own self-update transfer phase/
    percent (ota_http_get_esp_progress()) plus the persisted "last update"
    NVS record (ota_record.h: processor, version before/after, result,
    uptime_s), via the newer /api/ota/esp/status route. Both are queried
    independently and both are reported even if one of the two calls fails
    -- a Pico-only or ESP-only failure does not hide the other's result.

    Previously an HONEST GAP (through 2026-08-18): /api/ota/esp/status did
    not exist, so the ESP self-update's own progress and the persisted
    ota_record were real, C-level state with no HTTP route. That gap is now
    closed -- see ota_http_client.py's get_esp_status().

    NOT YET VERIFIED AGAINST REAL HARDWARE -- response parsing is
    unit-tested with mocked HTTP only.
    """
    resolved = _ota_resolve_host(host)

    try:
        pico = ota_http.get_pico_status(resolved)
        pico_str = (f"phase={pico.get('phase')!r} percent={pico.get('percent')} "
                    f"last_error={pico.get('last_error')!r}")
    except ota_http.OtaHttpError as exc:
        pico_str = f"error: {exc}"

    try:
        esp = ota_http.get_esp_status(resolved)
        last_update = esp.get("last_update")
        if last_update is None:
            last_update_str = "none (no ESP update has run this boot's NVS lifetime)"
        else:
            last_update_str = (f"processor={last_update.get('processor')!r} "
                                f"{last_update.get('version_before')!r}->"
                                f"{last_update.get('version_after')!r} "
                                f"success={last_update.get('success')} "
                                f"reason={last_update.get('reason')!r} "
                                f"uptime_s={last_update.get('uptime_s')}")
        esp_str = (f"phase={esp.get('phase')!r} percent={esp.get('percent')} "
                   f"last_update: {last_update_str}")
    except ota_http.OtaHttpError as exc:
        esp_str = f"error: {exc}"

    return f"pico relay: {pico_str} (host={resolved}) | esp self-update: {esp_str} (host={resolved})"


