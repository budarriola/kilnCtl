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
# HTTP, not the UART link. The tools below drive ota_http.c's
# /api/ota/{esp,pico} + /api/ota/pico/status endpoints over the
# board's own web server -- request construction and response
# parsing live in ota_http_client.py (unit-tested there with mocked HTTP
# responses; see tools/PcTools/tests/test_ota_http_client.py). NEVER
# exercised against real hardware from here -- no board is attached in CI or
# in this pass's dev environment. Live-board verification (does a real ESP
# accept these bytes end to end, do the interlock paths behave as
# documented) is still outstanding.
#
# ROUTE_TIER_ADMIN (the admin session http_auth.urlopen() carries) is the
# only gate on every route below, on or off. These routes used to ALSO
# require an AP-password HMAC challenge/response handshake
# (CommonFW/docs/UPDATE_PROTOCOL.md section 2) -- retired 2026-09-29
# (WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open when login off").
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
def _ota_resolve_host_with_source(host: Optional[str]) -> "tuple[str, str]":
    """Same resolution order as `_ota_resolve_host`, but also reports which
    branch produced the value ("explicit" / "STA IP" / "default") -- a
    mutating caller (bench_test_run, ota_matrix_run) should always be able
    to say which board address it actually targeted, since the "default"
    branch (`ota_http.OTA_AP_DEFAULT_HOST`, itself
    `host_resolve.resolve_default_host()` evaluated at import time) can be a
    stale/cached address rather than the board currently on the bench."""
    if host:
        return host, "explicit"
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip, "STA IP"
    except WifiUartQueryError:
        pass
    return ota_http.OTA_AP_DEFAULT_HOST, "default"


def _ota_resolve_host(host: Optional[str]) -> str:
    return _ota_resolve_host_with_source(host)[0]


@_srv._tool()
def ota_update_esp(image_path: str, host: Optional[str] = None) -> str:
    """Push a new ESP32-S3 firmware image over Wi-Fi -- POST /api/ota/esp.

    DESTRUCTIVE-ADJACENT: this streams `image_path` (a raw ESP-IDF .bin,
    e.g. KilnCtrl.bin) straight into the inactive OTA slot and sets it as
    the next boot partition on success. Refused by the board itself unless
    every interlock in UPDATE_PROTOCOL.md section 1 holds (kiln idle, no
    heater commanded, safety link healthy, temperature below the configured
    ceiling) -- a refusal comes back here as a specific error naming the
    unmet precondition, not a generic failure.

    ROUTE_TIER_ADMIN (the admin session http_auth.urlopen() carries) is the
    only auth this route requires -- the AP-password HMAC challenge/response
    scheme this used to also perform was retired 2026-09-29
    (WEB_AUTH_PLAN.md item 2b).

    On success the image is written and set as the boot partition, but stays
    PENDING_VERIFY until the board reboots AND main.c's
    ota_rollback_confirm_task() confirms NVS/safety-link/web-server are all
    up post-reboot -- this call does not reboot the board itself, and does
    not wait for or confirm that verification. Nothing here retries a
    partial write: a failure mid-transfer is left failed on both sides
    (ota_esp_do_transfer()'s single cleanup path aborts the OTA handle and
    releases the update mutex), and re-uploading is a fresh, separate call.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment).
    """
    resolved = _ota_resolve_host(host)
    try:
        result = ota_http.push_esp_image(resolved, image_path)
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
def ota_rollback_esp(host: Optional[str] = None) -> str:
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

    ROUTE_TIER_ADMIN is the only auth this route requires -- the
    AP-password HMAC challenge/response scheme it used to also perform was
    retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b).

    On success, the board has already persisted an ota_record (processor
    "esp", success=true, reason "rollback requested") and is rebooting into
    the previous image from a short-lived background task -- this call
    returns as soon as the board's response arrives, it does NOT wait for
    the reboot to finish or re-check the version afterward. Call this
    tool's caller's own version check (e.g. get_fw_version, once the board
    is back up) to confirm which image is actually running now.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment); no ESP32-S3 was available in
    this environment to actually trigger a reboot/rollback.
    """
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.rollback_esp(resolved)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - rollback accepted, was running version={body.get('version_before')!r} "
            f"(host={resolved}) -- board is rebooting into the previous image now; "
            f"re-check the version once it comes back up")


@_srv._tool()
def recovery_enter(host: Optional[str] = None, confirm: bool = False) -> str:
    """Deliberately reboot the ESP32-S3 into its RECOVERY image -- POST
    /api/ota/esp/recovery_boot (docs/OTA_SINGLE_SLOT_PLAN.md section 4). With
    one OTA slot, ota_rollback_esp() has nothing to roll back to; this is the
    way to reach the image that can take a fresh push (see the recovery_*
    tools, which only work once the board is running that image).

    Refuses unless `confirm is True` exactly. The board itself refuses (409)
    while a profile or autotune is running, while any relay is on or
    unreadable, on an unmet OTA interlock, when another update holds the
    mutex, on a partition table where the running image IS the factory
    partition, and when the recovery partition does not verify as a bootable
    image; none of those refusals changes anything on the board.
    ROUTE_TIER_ADMIN (the admin web session) is the only auth; no credential
    is printed.

    Selecting recovery erases otadata (stock IDF), so the board stays in
    recovery until recovery_exit sets it back to the application. The call
    returns when the board answers; it does not wait for the reboot.

    NOT VERIFIED AGAINST REAL HARDWARE -- request/response handling is
    unit-tested with mocked HTTP only.
    """
    if confirm is not True:
        return ("error: refused -- this reboots the board into the recovery image and leaves it there "
                "until recovery_exit; pass confirm=True (exactly True) to proceed")
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.recovery_boot_esp(resolved)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - recovery boot accepted, was running version={body.get('version_before')!r} "
            f"(host={resolved}) -- board is rebooting into the recovery image now; use recovery_status "
            f"once it is back up")


@_srv._tool()
def ota_recovery_exit_esp(host: Optional[str] = None) -> str:
    """Ask the ESP32-S3 to reboot right now to exit boot_guard.h's recovery
    mode -- POST /api/ota/esp/recovery_exit.

    Recovery mode means a recent update did not self-confirm within its
    window; the board already self-clears the counter that put it there
    within OTA_CONFIRM_POLL_MS of any boot (recovery-mode boots included),
    so the NEXT reboot lands back in normal mode on its own. This tool is
    the impatient/uncertain case: reboot right now instead of waiting for
    that background self-clear (or a watchdog) to do it.

    ROUTE_TIER_ADMIN is the only auth this route requires -- the
    AP-password HMAC challenge/response scheme it used to also perform was
    retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b). ALSO refused (403,
    "board is not in recovery mode") if the board is not currently in
    recovery mode.

    On success, the board is already rebooting from a short-lived
    background task -- this call returns as soon as the response arrives,
    it does NOT wait for the reboot to finish.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/
    response-parsing are unit-tested with mocked HTTP only; no ESP32-S3 was
    available in this environment to actually trigger recovery mode and
    exit it.
    """
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.recovery_exit_esp(resolved)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - recovery-mode exit accepted (host={resolved}) -- "
            f"board is rebooting now")


@_srv._tool()
def sw_reset_esp(confirm: bool = False, host: Optional[str] = None) -> str:
    """Reboot BOTH processors right now -- this ESP32-S3, and (since
    8b0e799a) the RP2040 safety processor IN PLACE, same firmware slot --
    POST /api/sw_reset. This is the sanctioned, non-JTAG way to reopen the
    safety processor's ~60s post-reset config_store write grace window: an
    operator (or a tool) who needs to commission a field while the Pico is
    ARMED (the ordinary standing state, in which config_store_write() refuses
    every field) uses this to get a fresh grace window, rather than reaching
    for debug_reset(peer="pico") over JTAG.

    `confirm` must be True or this refuses immediately with no request sent
    to the board at all -- same idiom as safety_set_rate_guard()'s own
    confirm gate. This is a consequential action (both processors reset,
    firing control genuinely drops for ~10-15s) and REQUIRES explicit intent
    every call; there is no "remember my answer".

    Also refused by the board itself (409, surfaced here as an error) while a
    firing/autotune is running or any zone's heater is commanded on -- same
    interlock factory_reset and the OTA update routes already use -- and the
    safety processor separately refuses ITS OWN half (relay armed, or a
    firmware transfer into it in flight) while still letting this ESP reboot;
    that per-processor outcome is in the returned detail text, not summarized
    away.

    IMPORTANT, read before calling: this call may latch an S6a
    (SAFETY_TRIP_MAIN_FAULT) trip on the safety processor -- the theory is
    that this ESP's isolated fault line to it goes undefined across this
    ESP's own reset, which safety_guards.c's S6a block reads as a main-fault
    unconditionally (there is no grace window over S6a, only over S6b). It
    was OBSERVED NOT TO on the bench (OT-B01, 2026-09-30 and 2026-10-01,
    run 20261001T072647Z_ota: trip_reason 0, mask 0); S6a sightings came
    from JTAG/flash dual resets. Check the trip state afterward. This tool does NOT
    clear that trip -- S6a exists to report exactly this event, and
    auto-clearing it from the same call that caused it would defeat the
    point. REQUIRED FOLLOW-UP before heating: once safety_get_status()/
    safety_get_diag() show the link back up, confirm trip_mask is ONLY
    SAFETY_TRIP_MAIN_FAULT (bit 5, 0x0020 -- per
    link_frame_trip_mask_for_reason(), mask is 1 << (trip_reason - 1) and
    SAFETY_TRIP_MAIN_FAULT is trip_reason 6, so bit 5; 0x0040 is bit 6,
    SAFETY_TRIP_LINK_DEAD (S6b) -- decode any OTHER bit and stop, do not
    clear) and then call safety_clear_trip() explicitly.

    ROUTE_TIER_ADMIN is the only auth this route requires -- the
    AP-password HMAC challenge/response scheme it used to also perform was
    retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b).

    No configuration is erased or changed on either processor by this call
    -- contrast the danger-zone factory_reset scopes.

    NOT YET VERIFIED AGAINST REAL HARDWARE by this repo's own automated test
    suite -- request construction/response-parsing are unit-tested with
    mocked HTTP only; see test_ota_http_client.py.
    """
    if not confirm:
        return ("error: refused -- confirm=True is required. This reboots BOTH processors right "
                "now and may latch an S6a main-fault trip on the safety processor (not observed on "
                "the bench for sw_reset, 2026-10-01); if one latches you must "
                "clear it yourself afterward (safety_clear_trip(), only once trip_mask is confirmed "
                "to be exactly 0x0020). No request was sent to the board.")
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.sw_reset(resolved)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - sw_reset accepted (host={resolved}); board detail: {body.get('detail')!r} -- "
            f"expect ~10-15s unreachable, then an S6a trip to clear with safety_clear_trip() "
            f"once trip_mask is confirmed to be exactly 0x0020")


@_srv._tool()
def ota_update_pico(image_path: str, host: Optional[str] = None,
                     force_version: bool = False) -> str:
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

    ROUTE_TIER_ADMIN is the only auth this route requires -- the
    AP-password HMAC challenge/response scheme it used to also perform was
    retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b).

    TODO.md 9.4: if the staged image declares a link protocol version
    different from the one this ESP currently speaks, the board refuses
    with a 409 naming both versions (surfaced here as
    OtaPicoProtocolVersionMismatch) BEFORE the relay ever starts -- this is
    a proactive, earlier warning on top of, never instead of, the Pico's
    own UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE refusal, which still runs
    regardless. Pass `force_version=True` only after a human operator has
    confirmed the mismatch is intentional (e.g. deliberately testing an
    older/newer link protocol); never set it from an unattended path.

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
    tool; only request construction/response-parsing are unit-tested,
    with mocked HTTP.
    """
    resolved = _ota_resolve_host(host)
    try:
        result = ota_http.push_pico_image(resolved, image_path,
                                           force_version=force_version)
    except ota_http.OtaPicoProtocolVersionMismatch as exc:
        return (f"error: protocol version mismatch -- image declares link protocol "
                f"{exc.image_protocol_version}, this board speaks {exc.esp_protocol_version} "
                f"(HTTP {exc.status}) (host={resolved}). Retry with force_version=True only "
                f"after confirming this is intentional.")
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
        # protocol_version_known/protocol_version/protocol_min_compatible/
        # protocol_compatible come straight from safety_link_get_peer_version_status()
        # (ota_http.c's ota_pico_status_get_handler()) -- the ESP's live view
        # of what protocol version the Pico is actually running right now,
        # independent of which image is staged/relaying. A caller pushing a
        # new Pico image should check this BEFORE relying on the two
        # processors being able to talk afterward; a hard mismatch here is
        # surfaced with an explicit INCOMPATIBLE tag rather than folded into
        # the same prose as a healthy status, so a caller cannot mistake one
        # for the other by skimming.
        if pico.get("protocol_version_known"):
            compatible = bool(pico.get("protocol_compatible"))
            tag = "compatible" if compatible else "INCOMPATIBLE"
            pico_str += (f" protocol_version={pico.get('protocol_version')} "
                         f"min_compatible={pico.get('protocol_min_compatible')} "
                         f"[{tag}]")
            if not compatible:
                pico_str = f"INCOMPATIBLE PROTOCOL VERSION: {pico_str}"
        else:
            pico_str += " protocol_version=unknown (no version handshake yet)"
    except ota_http.OtaHttpError as exc:
        pico_str = f"error: {exc}"

    try:
        esp = ota_http.get_esp_status(resolved)
        last_update = esp.get("last_update")
        if last_update is None:
            last_update_str = "none (no ESP update has run this boot's NVS lifetime)"
        else:
            # downgrade_known/is_downgrade added 2026-09-24
            # (UPDATE_PROTOCOL.md's "a downgrade is allowed but logged as
            # such" bullet) -- is_downgrade is only meaningful when
            # downgrade_known is true, same "unknown is never a real
            # answer" convention as protocol_version_known above.
            if last_update.get("downgrade_known"):
                downgrade_str = "DOWNGRADE" if last_update.get("is_downgrade") else "upgrade/same"
            else:
                downgrade_str = "unknown (version strings not comparable)"
            last_update_str = (f"processor={last_update.get('processor')!r} "
                                f"{last_update.get('version_before')!r}->"
                                f"{last_update.get('version_after')!r} "
                                f"[{downgrade_str}] "
                                f"success={last_update.get('success')} "
                                f"reason={last_update.get('reason')!r} "
                                f"uptime_s={last_update.get('uptime_s')}")
        esp_str = (f"phase={esp.get('phase')!r} percent={esp.get('percent')} "
                   f"last_update: {last_update_str}")
    except ota_http.OtaHttpError as exc:
        esp_str = f"error: {exc}"

    return f"pico relay: {pico_str} (host={resolved}) | esp self-update: {esp_str} (host={resolved})"


