#!/usr/bin/env python3
"""ota_http_client.py -- pure HTTP client for the ESP's OTA update endpoints
(firmware/KilnFW/App/drivers/http/ota_http.c), used by mcp_server.py's ota_* tools.

Talks straight HTTP to /api/ota/{esp,pico} and GET
/api/ota/pico/status -- same "stdlib urllib.request, no framework" convention
gui.py already uses for this board's other HTTP surfaces (Wi-Fi settings
popup, Rules editor -- see gui.py's _wifi_default_host()/
_wifi_http_error_text()). Kept in its own module, separate from
mcp_server.py, specifically so the request-construction /
response-parsing logic here can be unit-tested with mocked HTTP responses
(tools/PcTools/tests/test_ota_http_client.py) with no real socket and no
live board required.

Wire contract source of truth: firmware/KilnFW/App/drivers/http/ota_http.h/.c and
firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 2 (auth) and section 3 (ESP
transfer)/section 9.5-era pico staging. Mirrored here, not re-derived:

  Every route below used to also require an AP-password HMAC challenge/
  response handshake on top of ROUTE_TIER_ADMIN. Retired 2026-09-29
  (WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open when login off"):
  route_tier_table.h's ADMIN tier is now the sole gate, on or off, same as
  every other admin route -- every call below goes through
  http_auth.urlopen() (the ordinary admin session seam) with no nonce, no
  MAC, no X-Ota-Mac header.

  POST /api/ota/esp,  raw .bin body
       -> 200 {"ok":true,"bytes":N,"partition":"...","version":"..."}
  POST /api/ota/pico, same body shape, image is SaftyFW's raw .bin
       -> 202 {"ok":true,"status":"relay_started","bytes":N,"crc32":"0x..."}
  GET  /api/ota/pico/status    -> 200 {"phase":"...","percent":N,"last_error":"..."}
  GET  /api/ota/esp/status     -> 200 {"phase":"...","percent":N,"last_update":null|{...}}
  POST /api/ota/esp/rollback, empty body -> 200 {"ok":true,"status":"rebooting",
       "version_before":"..."}. Explicit revert to the previous OTA image;
       reboots the board shortly after responding.
  POST /api/ota/esp/recovery_exit, empty body -> 200 {"ok":true,
       "status":"rebooting"}, or 403 "board is not in recovery mode" if the
       board is not currently in boot_guard.h's recovery mode. Reboots the
       board shortly after responding, same as rollback above.

Failure responses (400/403/409/500) are PLAIN TEXT
(httpd_resp_send_err()/httpd_resp_set_status()+httpd_resp_send()), not JSON --
so this client reads a non-2xx body as text and surfaces it verbatim in
OtaHttpError.detail rather than trying to json.loads() it. One exception
(TODO.md 9.4): POST /api/ota/pico's 409 protocol_version_mismatch refusal IS
JSON, and this module detects that one shape and raises the typed
OtaPicoProtocolVersionMismatch instead -- every other 4xx/5xx on this surface
is still plain text.

CLOSED GAP (was open through 2026-08-18): there used to be no HTTP endpoint
exposing ota_http_get_esp_progress() (the ESP self-update's own progress
counter) or the persisted ota_record (App/drivers/persist/ota_record.h's "last
update" NVS blob). `GET /api/ota/esp/status` (App/drivers/http/ota_http.c) now
covers both -- get_esp_status() below reads it. `last_update` is `null`
when no update has ever run this NVS lifetime (ota_record_load() found
nothing), or an object with ota_record_t's fields (processor,
version_before, version_after, success, reason, uptime_s) when one exists.
Build/test-verified only -- see ota_status() in mcp_server.py and
TODO.md 9.6a for the same "no physical board exercised" caveat this whole
module already carries.
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import urllib.error
import urllib.request

from . import host_resolve, http_auth
from dataclasses import dataclass, field
from typing import Optional

#: Every push/rollback/recovery call is logged here -- image SHA-256 and
#: outcome on success, the board's refusal reason on failure. NEVER a
#: credential: nothing below ever logs a password or session cookie. See
#: test_ota_http_client.py's LoggingTest for the negative proof.
log = logging.getLogger(__name__)

#: Short requests: pico status poll.
OTA_HTTP_TIMEOUT_S = 8.0
#: The ESP path holds the HTTP connection open for the whole streamed write
#: (ota_http.c streams straight to flash, no staging) -- a ~1.1-2 MB image
#: over Wi-Fi. Generous margin over what a healthy LAN needs.
OTA_ESP_UPLOAD_TIMEOUT_S = 180.0
#: The Pico path only has to cover the FAST Wi-Fi-speed stage into pico_img
#: -- ota_http.c responds 202 as soon as staging finishes, without waiting
#: for the ~35s+ relay over the isolated UART link. That relay is polled
#: back separately via get_pico_status().
OTA_PICO_STAGE_TIMEOUT_S = 60.0

#: Mirrors gui.py's _WIFI_AP_DEFAULT_HOST -- the board's own fallback-AP
#: address, reachable when nothing has ever joined a home network yet.
OTA_AP_DEFAULT_HOST = host_resolve.resolve_default_host()  # was a hardcoded "192.168.4.1"
class OtaHttpError(Exception):
    """Raised for any transport or protocol failure talking to the board's
    OTA endpoints -- unreachable host, a non-2xx response, or a response
    that doesn't parse as the expected JSON. Carries the raw detail text the
    board sent (if any), so a caller can surface the board's own specific
    refusal reason (an interlock message, "wrong password", "an update is
    already in progress", ...) instead of a generic failure string."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


class OtaPicoProtocolVersionMismatch(OtaHttpError):
    """POST /api/ota/pico refused with 409 {"error":"protocol_version_mismatch"}
    (ota_http_pico.c's TODO.md 9.4 warning): the uploaded SaftyFW image
    declares a KILNLINK_PROTOCOL_VERSION different from the one this ESP
    currently speaks. This is a proactive, EARLIER warning on top of, never
    instead of, the Pico's own UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE
    refusal -- that floor still runs regardless of this exception. Retry
    with `push_pico_image(..., force_version=True)` only after a human has
    confirmed the mismatch is intentional; the board's response body is
    still available verbatim in `.detail` for logging."""

    def __init__(self, message: str, status: int, detail: str,
                 image_protocol_version: int, esp_protocol_version: int):
        super().__init__(message, status, detail)
        self.image_protocol_version = image_protocol_version
        self.esp_protocol_version = esp_protocol_version


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def _http_error_detail(exc: Exception) -> tuple[Optional[int], str]:
    """Same normalization gui.py's _wifi_http_error_text() does, minus the
    Tk-facing formatting -- kept here so callers get a status code back too,
    not just a display string."""
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace").strip()
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


@dataclass
class OtaPushResult:
    ok: bool
    status_code: int
    body: dict = field(default_factory=dict)


def _establish_admin_session(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> None:
    """One cheap ADMIN-tier GET through http_auth.urlopen() before a large
    body POST, so a 401 is seen and handled (login + retry) on a tiny request.
    GET /api/ota/interlock is ROUTE_TIER_ADMIN (route_tier_table.h, bench
    confirmed 2026-10-03: an OPEN-tier GET such as /api/ota/esp/status does NOT
    establish a session) and a small, side-effect-free read.
    Firmware's http_auth_refusal_should_close() closes an UNAUTHENTICATED
    refusal whose unread body exceeds 4096 B instead of draining it, so a
    multi-MB POST made with no session never yields a readable 401: urllib
    sees ConnectionResetError and http_auth's login-and-retry never fires.
    Best effort for transport errors/timeouts (swallowed and logged; the real
    POST then reports whatever the board does). A 401/403 answer to this probe
    is NOT swallowed: it means login failed or the role is not ADMIN, so it is
    re-raised as OtaHttpError(status) before any upload starts."""
    req = urllib.request.Request(_url(host, "/api/ota/interlock"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            resp.read()
    except urllib.error.HTTPError as exc:
        if exc.code in (401, 403):
            raise OtaHttpError(f"/api/ota/interlock session probe refused: HTTP {exc.code}", exc.code) from exc
        log.warning("OTA pre-push session probe failed: host=%s: HTTP %s", host, exc.code)
    except Exception as exc:  # noqa: BLE001 -- see docstring
        log.warning("OTA pre-push session probe failed: host=%s: %s: %s",
                    host, type(exc).__name__, exc)


def _push_image(host: str, path: str, endpoint: str,
                 timeout: float, extra_headers: Optional[dict] = None) -> OtaPushResult:
    """Shared body of push_esp_image()/push_pico_image(): validate the local
    file and POST the raw bytes. ROUTE_TIER_ADMIN (the admin session
    http_auth.urlopen() already carries) is the only gate on this route --
    the AP-password HMAC challenge/response scheme this used to also perform
    was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b). Nothing here retries a
    partial write: any exception anywhere in this function propagates as-is,
    and a caller must treat that as "unknown whether the board received
    anything usable" -- re-uploading is a fresh, explicit action, never
    something this function does on its own behalf.
    """
    if not os.path.isfile(path):
        raise OtaHttpError(f"no such file: {path}")
    size = os.path.getsize(path)
    if size == 0:
        raise OtaHttpError(f"image file is empty: {path}")

    _establish_admin_session(host)

    with open(path, "rb") as f:
        data = f.read()

    #: Identifies exactly what image was pushed. Logged before the request
    #: so a failed/hung transfer still leaves a record of what was attempted.
    image_sha256 = hashlib.sha256(data).hexdigest()
    log.info("OTA push starting: endpoint=%s host=%s path=%s size=%d sha256=%s",
              endpoint, host, path, size, image_sha256)

    headers = {
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(data)),
    }
    if extra_headers:
        headers.update(extra_headers)
    req = urllib.request.Request(
        _url(host, endpoint),
        data=data,
        method="POST",
        headers=headers,
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            status_code = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("OTA push refused: endpoint=%s host=%s sha256=%s status=%s detail=%s",
                    endpoint, host, image_sha256, status_code, detail)
        # ota_http_pico.c's TODO.md 9.4 protocol-version warning is the one
        # refusal on this surface that comes back as JSON rather than plain
        # text (this module's own header comment on 400/403/409/500 being
        # plain text predates it) -- detect it here so a caller gets a
        # typed exception with both version numbers instead of having to
        # re-parse `.detail` itself.
        if status_code == 409:
            try:
                detail_obj = json.loads(detail)
            except Exception:
                detail_obj = None
            if isinstance(detail_obj, dict) and detail_obj.get("error") == "protocol_version_mismatch":
                raise OtaPicoProtocolVersionMismatch(
                    f"{endpoint} refused: image link protocol "
                    f"{detail_obj.get('image_protocol_version')} != this board's "
                    f"{detail_obj.get('esp_protocol_version')}",
                    status_code, detail,
                    image_protocol_version=detail_obj.get("image_protocol_version"),
                    esp_protocol_version=detail_obj.get("esp_protocol_version"),
                ) from exc
        raise OtaHttpError(f"{endpoint} refused: HTTP {status_code}: {detail}", status_code,
                            detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("OTA push failed (unreachable): endpoint=%s host=%s sha256=%s detail=%s",
                    endpoint, host, image_sha256, detail)
        raise OtaHttpError(f"{endpoint} unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        log.warning("OTA push response unparseable: endpoint=%s host=%s sha256=%s body=%r",
                    endpoint, host, image_sha256, body_text)
        raise OtaHttpError(f"{endpoint} response was not valid JSON: {body_text!r}",
                            status_code, body_text) from exc

    result = OtaPushResult(ok=bool(body.get("ok")), status_code=status_code, body=body)
    if result.ok:
        log.info("OTA push accepted: endpoint=%s host=%s sha256=%s status=%d body=%s",
                  endpoint, host, image_sha256, status_code, body)
    else:
        log.warning("OTA push reported failure: endpoint=%s host=%s sha256=%s body=%s",
                    endpoint, host, image_sha256, body)
    return result


def push_esp_image(host: str, path: str,
                    timeout: float = OTA_ESP_UPLOAD_TIMEOUT_S) -> OtaPushResult:
    """POST /api/ota/esp -- streams the raw ESP-IDF .bin, holding the HTTP
    connection open for the whole transfer (ota_esp_do_transfer() streams
    straight to flash as the body arrives; this path is synchronous end to
    end, unlike the Pico path below). On success, `body` is
    {"ok":true,"bytes":N,"partition":"...","version":"..."}.

    The new image is left PENDING_VERIFY until App/main.c's
    ota_rollback_confirm_task() confirms it after a reboot
    (UPDATE_PROTOCOL.md section 3) -- this call only reports that the
    write+flash succeeded, not that the board is now running the new image.
    A caller still needs to reboot the board and re-check its version
    afterward.
    """
    return _push_image(host, path, "/api/ota/esp", timeout)


def push_pico_image(host: str, path: str,
                     timeout: float = OTA_PICO_STAGE_TIMEOUT_S,
                     force_version: bool = False) -> OtaPushResult:
    """POST /api/ota/pico -- streams the raw SaftyFW .bin into the `pico_img`
    staging partition at Wi-Fi speed, then returns as soon as staging
    finishes (202 Accepted,
    {"ok":true,"status":"relay_started","bytes":N,"crc32":"0x..."}) -- it does
    NOT wait for the ~35s+ relay over the isolated UART link to the RP2040 to
    finish. A 202 here means "upload accepted and the relay task started",
    never "the safety processor is now running the new image" -- poll
    get_pico_status() afterward for the actual relay outcome.

    Raises `OtaPicoProtocolVersionMismatch` (TODO.md 9.4) if the image
    declares a KILNLINK_PROTOCOL_VERSION different from the one this board
    currently speaks and `force_version` is False -- the board never even
    starts the relay in that case, so nothing was written to the safety
    processor. `force_version=True` sends X-Ota-Force-Version: 1, which
    relays the image anyway despite a known mismatch; a caller should set
    it only after a human has confirmed that intentionally. Never pass
    `force_version=True` from an unattended/automatic path -- that
    confirmation is the entire point of this gate.
    """
    extra_headers = {"X-Ota-Force-Version": "1"} if force_version else None
    return _push_image(host, path, "/api/ota/pico", timeout,
                        extra_headers=extra_headers)


def get_pico_status(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/ota/pico/status -- unauthenticated poll-back for the
    background relay task's progress (ota_pico_relay.c). Returns
    {"phase": "...", "percent": 0-100, "last_error": "..."} straight from the
    board; phase is one of ota_pico_relay_phase_str()'s strings (idle,
    begin, erasing, sending, retransmit, finishing, done, failed)."""
    req = urllib.request.Request(_url(host, "/api/ota/pico/status"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise OtaHttpError(f"pico status request failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"pico status response was not valid JSON: {body_text!r}") from exc


def rollback_pico(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/pico/rollback -- explicit "revert the safety
    processor's bootloader to its previous slot right now"
    (App/drivers/http/ota_http_pico.c's ota_pico_rollback_post_handler()).
    Like push_pico_image(), this is fire-and-forget at Wi-Fi speed: the
    handler starts a background task (ota_pico_rollback_task()) that does
    the actual isolated-link work and returns 202 Accepted,
    {"ok":true,"status":"rollback_started"} as soon as that task is
    launched -- it does NOT wait for the relay to the RP2040 to finish.
    Poll get_pico_rollback_status() afterward for the real outcome.

    Refused the same way a Pico push is: no/insufficient admin session
    (401/403), an unmet interlock (409 -- not idle, a trip pending, a
    concurrent update/rollback already holding the mutex), all raised as
    OtaHttpError with the board's plain-text reason in `.detail`. The
    AP-password HMAC challenge/response this used to also require was
    retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b) -- ROUTE_TIER_ADMIN (the
    admin session http_auth.urlopen() carries) is the only gate now.
    """
    req = urllib.request.Request(
        _url(host, "/api/ota/pico/rollback"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("Pico OTA rollback requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("Pico OTA rollback refused: host=%s status=%s detail=%s", host, status_code, detail)
        raise OtaHttpError(f"/api/ota/pico/rollback refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("Pico OTA rollback failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/ota/pico/rollback unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/ota/pico/rollback response was not valid JSON: {body_text!r}") from exc
    log.info("Pico OTA rollback accepted: host=%s body=%s", host, body)
    return body


def get_pico_rollback_status(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/ota/pico/rollback/status -- unauthenticated poll-back for
    the background rollback task (ota_pico_rollback_task(),
    ota_pico_rollback_status_get_handler()). Returns
    {"ok": bool, "status": "idle"|"pending"|"rebooting"|"link_down"|
    "send_failed"|"refused"|"unknown", "detail": "..."} straight from the
    board (ota_http_pico_rollback_format_body()) -- "rebooting" is the only
    success outcome and is reported only once BOTH the peer's boot_id and
    its build identity were observed to change, i.e. a real reboot into a
    different image was actually seen on the wire; "refused" also carries
    a numeric "reason_code" mirroring KILNLINK_ROLLBACK_RESULT_REASON_*
    (kilnlink_rollback_result.h)."""
    req = urllib.request.Request(_url(host, "/api/ota/pico/rollback/status"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise OtaHttpError(f"pico rollback status request failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"pico rollback status response was not valid JSON: {body_text!r}") from exc


def get_esp_status(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/ota/esp/status -- unauthenticated poll-back for the ESP's own
    self-update transfer (ota_http_get_esp_progress()) plus the persisted
    "last update" NVS record (ota_record.h). Returns
    {"phase": "...", "percent": 0-100, "last_update": null | {...}} straight
    from the board; phase is one of "idle"/"verifying"/"writing"/
    "finalizing"/"done"/"failed" (ota_http.c's esp_phase_str()). last_update
    is None (JSON null) until the first ESP update has ever run this NVS
    lifetime; once one has, it is a dict with processor/version_before/
    version_after/success/reason/uptime_s/image_sha256/downgrade_known/
    is_downgrade, mirroring ota_record_t (downgrade_known/is_downgrade added
    2026-09-24, UPDATE_PROTOCOL.md's "a downgrade is allowed but logged as
    such" bullet -- ota_version_compare()'s best-effort string comparison;
    is_downgrade is only meaningful when downgrade_known is true, same
    "0/unknown is never a real answer" rule as protocol_version_known
    elsewhere on this board)."""
    req = urllib.request.Request(_url(host, "/api/ota/esp/status"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise OtaHttpError(f"esp status request failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"esp status response was not valid JSON: {body_text!r}") from exc


def rollback_esp(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/rollback -- explicit "revert to the previous image
    right now" (App/drivers/http/ota_http.c's ota_esp_rollback_post_handler()).
    Unlike push_esp_image()/push_pico_image(), there is no file to send --
    the body is empty. ROUTE_TIER_ADMIN (the admin session
    http_auth.urlopen() carries) is the only gate; the AP-password HMAC
    challenge/response this route used to also require was retired
    2026-09-29 (WEB_AUTH_PLAN.md item 2b).

    Refused the same way an update push is: no/insufficient admin session
    (401/403), an unmet interlock (409, kiln not idle/cool or similar -- see
    ota_http_check_interlocks()), a concurrent update/rollback already
    holding the mutex (409), or -- specific to this route -- no previous
    valid image to roll back to (409, "no previous valid image to roll back
    to", from esp_ota_check_rollback_is_possible()). All of these raise
    OtaHttpError with the board's specific plain-text reason in `.detail`.

    On success (200), the board has already appended an ota_record and is
    about to reboot into the previous image from a short-lived background
    task (ota_rollback_reboot_task()) -- this call returns as soon as that
    response is received, it does NOT wait for the reboot or for the board
    to come back up running the older version. Returns the parsed JSON body,
    {"ok": true, "status": "rebooting", "version_before": "<version>"}.
    """
    req = urllib.request.Request(
        _url(host, "/api/ota/esp/rollback"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("OTA rollback requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("OTA rollback refused: host=%s status=%s detail=%s", host, status_code, detail)
        raise OtaHttpError(f"/api/ota/esp/rollback refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("OTA rollback failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/ota/esp/rollback unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        log.warning("OTA rollback response unparseable: host=%s body=%r", host, body_text)
        raise OtaHttpError(f"/api/ota/esp/rollback response was not valid JSON: {body_text!r}") from exc
    if body.get("ok"):
        log.info("OTA rollback accepted: host=%s body=%s", host, body)
    else:
        log.warning("OTA rollback reported failure: host=%s body=%s", host, body)
    return body


def recovery_boot_esp(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/recovery_boot -- deliberately reboot the board into its
    RECOVERY image (App/drivers/http/ota_http_recovery.c's
    ota_recovery_boot_post_handler(); docs/OTA_SINGLE_SLOT_PLAN.md section 4).
    With one OTA slot, ota_rollback_esp() has nothing to roll back to; this is
    the way to reach the image that can take a fresh push.

    ROUTE_TIER_ADMIN is the only gate. The board refuses with 409 while a
    firing/autotune runs, while any relay is on or its state is unreadable,
    on an unmet OTA interlock, while another update holds the mutex, when the
    running image is itself the factory partition, or when the recovery
    partition does not verify as a bootable image (nothing is written in any
    refusal). On success (200) the boot target is already set and a short
    task is about to reboot; this call does NOT wait for the reboot. Returns
    {"ok": true, "status": "rebooting", "target": "recovery",
    "version_before": "<version>"}. Empty body, no credential in the request.
    """
    req = urllib.request.Request(
        _url(host, "/api/ota/esp/recovery_boot"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("recovery boot requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("recovery boot refused: host=%s status=%s detail=%s", host, status_code, detail)
        raise OtaHttpError(f"/api/ota/esp/recovery_boot refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("recovery boot failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/ota/esp/recovery_boot unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/ota/esp/recovery_boot response was not valid JSON: {body_text!r}") from exc
    if body.get("ok"):
        log.info("recovery boot accepted: host=%s body=%s", host, body)
    else:
        log.warning("recovery boot reported failure: host=%s body=%s", host, body)
    return body


def recovery_exit_esp(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/recovery_exit -- ask the board to reboot right now
    to exit boot_guard.h's recovery mode, rather than waiting for it to
    self-clear (App/drivers/http/ota_http.c's ota_recovery_exit_post_handler()).

    ROUTE_TIER_ADMIN (the admin session http_auth.urlopen() carries) is the
    only gate; the AP-password HMAC challenge/response this route used to
    also require was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b). Refused
    (401/403) for no/insufficient admin session, and ALSO refused (403,
    "board is not in recovery mode") if the board is not currently in
    recovery mode (ota_recovery_exit_post_handler()'s doc comment).

    On success (200), the board is already rebooting from a short-lived
    background task -- this call returns as soon as the response arrives,
    it does NOT wait for the reboot to finish. Returns the parsed JSON body,
    {"ok": true, "status": "rebooting"}.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment); no ESP32-S3 was available in
    this environment to actually trigger recovery mode and exit it.
    """
    req = urllib.request.Request(
        _url(host, "/api/ota/esp/recovery_exit"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("OTA recovery-exit requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("OTA recovery-exit refused: host=%s status=%s detail=%s",
                    host, status_code, detail)
        raise OtaHttpError(f"/api/ota/esp/recovery_exit refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("OTA recovery-exit failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/ota/esp/recovery_exit unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        log.warning("OTA recovery-exit response unparseable: host=%s body=%r", host, body_text)
        raise OtaHttpError(
            f"/api/ota/esp/recovery_exit response was not valid JSON: {body_text!r}") from exc
    if body.get("ok"):
        log.info("OTA recovery-exit accepted: host=%s body=%s", host, body)
    else:
        log.warning("OTA recovery-exit reported failure: host=%s body=%s", host, body)
    return body


def boot_guard_reset_esp(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/boot_guard_reset -- the tool-driven half of
    docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md's fix
    (App/drivers/http/ota_http_recovery.c's ota_boot_guard_reset_post_handler()).

    Tells the board directly "a tool just performed a deliberate flash and
    independently confirmed the new build is running" -- bypassing
    boot_confirm_is_healthy()'s one-shot, never-retried NVS snapshot that a
    board can sample during a transient window right after a flash-triggered
    reset, walking an entirely healthy board toward recovery mode after a
    few ordinary reflashes. ONLY call this after your own independent
    verification that the flash landed and the new build is actually
    running -- see mcp_server_flash.py's flash_firmware(), the sanctioned
    caller, which calls this only once `_verify_flash_landed()` returns ""
    (full, unambiguous success), never on a failed, unverified, or merely
    warned-about flash. A board that was just flashed with something broken
    must still be free to walk into recovery mode on its own.

    ROUTE_TIER_ADMIN (the admin session http_auth.urlopen() carries) is the
    only gate; the AP-password HMAC challenge/response this route used to
    also require was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b). Unlike
    recovery_exit_esp(), the board does NOT need to be in recovery mode for
    this to succeed (the common case here is an ORDINARY, non-recovery-mode
    board), and the board does NOT reboot afterward -- its only effect is
    the NVS clear.

    Returns the parsed JSON body, {"ok": bool, "boot_count": int}. `ok` is
    true only once the board's own read-back confirmed the clear actually
    landed (boot_guard_reset_counter()'s verified-with-retry contract, same
    as boot_guard_mark_healthy()) -- `ok: false` means the write may have
    reported success internally but did not verify, exactly the class of
    lying write CLAUDE.md's boot_guard section warns never to trust the
    return code of alone. A caller MUST check `ok`, not just that this call
    did not raise: a 200 with ok:false is not success.
    """
    req = urllib.request.Request(
        _url(host, "/api/ota/esp/boot_guard_reset"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("OTA boot_guard_reset requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("OTA boot_guard_reset refused: host=%s status=%s detail=%s", host, status_code, detail)
        raise OtaHttpError(f"/api/ota/esp/boot_guard_reset refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("OTA boot_guard_reset failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/ota/esp/boot_guard_reset unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        log.warning("OTA boot_guard_reset response unparseable: host=%s body=%r", host, body_text)
        raise OtaHttpError(
            f"/api/ota/esp/boot_guard_reset response was not valid JSON: {body_text!r}") from exc
    if body.get("ok"):
        log.info("OTA boot_guard_reset accepted and VERIFIED: host=%s body=%s", host, body)
    else:
        log.warning("OTA boot_guard_reset did NOT verify (lying-write class -- see CLAUDE.md's "
                    "boot_guard section): host=%s body=%s", host, body)
    return body


def get_boot_guard_status(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/boot_guard -- admin-authenticated diagnostics for boot_guard's
    recovery-mode counter (the read goes through http_auth.urlopen(), the
    same ADMIN-session seam every other admin-tier tool uses, and the route
    401s once web auth is on) (App/drivers/http/ota_http_recovery.c's
    ota_boot_guard_status_get_handler()), added alongside the reset route
    above so this class of fix is verifiable without a JTAG memory read of
    s_bg (docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md
    named exactly that as the previous, painful verification path).

    Returns the parsed JSON body, {"boot_count": int, "recovery_mode": bool}.
    """
    req = urllib.request.Request(_url(host, "/api/boot_guard"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/boot_guard refused: HTTP {status_code}: {detail}", status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/boot_guard unreachable: {detail}") from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/boot_guard response was not valid JSON: {body_text!r}") from exc


def sw_reset(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/sw_reset -- reboot BOTH processors: this ESP32-S3, and (since
    8b0e799a) the RP2040 safety processor IN PLACE, same firmware slot, via
    the wire command SAFETY_CMD_REBOOT (0x29) relayed over the isolated UART
    link. This is the sanctioned non-JTAG way to get the Pico's config_store
    back into its ~60s post-reset write grace window -- see
    firmware/KilnFW/App/drivers/http/sw_reset_http.c's module doc comment for
    the full rationale, and CLAUDE.md's "Anything involving the boards"
    section for why JTAG (debug_reset(peer="pico")) is not the only path.

    No config on either processor is touched by this call itself. The Pico
    may REFUSE its half (heating relay armed, or a firmware transfer into it
    in flight) while the ESP still reboots -- the two halves are reported
    independently in the returned dict, never as one undifferentiated "ok".
    SAFETY_CMD_ROLLBACK must never be used for this: it boots the OTHER,
    possibly-refused bootloader slot, not the running one.

    ROUTE_TIER_ADMIN (the admin session http_auth.urlopen() carries) is the
    only gate; the AP-password HMAC challenge/response this route used to
    also require was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b).

    IMPORTANT -- this call was expected to latch an S6a (SAFETY_TRIP_MAIN_FAULT)
    trip on the safety processor, but it has been OBSERVED NOT TO (OT-B01,
    run 20261001T072647Z_ota, 2026-10-01, and 2026-09-30: real dual reset
    confirmed, trip_reason 0, mask 0). The theory was that this ESP's
    isolated fault line to it floats across this ESP's own reset, which
    safety_guards.c's S6a block reads as a main-fault after a 200 ms debounce
    (no grace window over S6a, only over S6b); the trip-6 cases actually
    seen came from JTAG/flash dual resets, a different path. Absence of a
    trip after this call is not a fault; check the trip state rather than
    assuming either outcome (OT-B01 records which happened). This call does NOT clear any trip -- S6a exists to
    report exactly this event, and auto-clearing it from the same call that
    caused it would defeat that purpose. A REQUIRED follow-up before heating
    is a separate, explicit call to clear the trip (POST
    /api/safety/clear_trip) once the safety link is confirmed back up and the
    trip mask is confirmed to be ONLY SAFETY_TRIP_MAIN_FAULT (bit 5, 0x0020;
    per link_frame_trip_mask_for_reason() mask = 1 << (trip_reason - 1) and
    SAFETY_TRIP_MAIN_FAULT is trip_reason 6, so bit 5 -- 0x0040 is bit 6,
    SAFETY_TRIP_LINK_DEAD/S6b) -- never clear a trip carrying any other bit
    without understanding it first.

    On success (200), this ESP is already committed to rebooting itself from
    a short-lived background task -- this call returns as soon as the
    response arrives, before that reboot actually happens, so the connection
    dropping out from under a caller mid-read is expected, not an error (see
    sw_reset_http.c's own reboot-task comment). The response body is plain
    text (not JSON, unlike every other route in this module) -- reported
    back to the caller as {"ok": True, "detail": "<body text>"} on any 2xx,
    since sw_reset_http.c's own text already states, per-processor, whether
    the Pico accepted, refused (and why), or never confirmed; a caller that
    wants the Pico's own outcome should parse that text or, more robustly,
    poll safety_get_diag()/get_fw_version() (boot_id) once the ESP is back up.

    NOT YET VERIFIED AGAINST REAL HARDWARE by this module's own test suite --
    request construction/response-parsing are unit-tested with mocked HTTP
    only; see test_ota_http_client.py.
    """
    req = urllib.request.Request(
        _url(host, "/api/sw_reset"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("sw_reset requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            status_code = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("sw_reset refused: host=%s status=%s detail=%s", host, status_code, detail)
        raise OtaHttpError(f"/api/sw_reset refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("sw_reset failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/sw_reset unreachable: {detail}") from exc

    log.info("sw_reset accepted: host=%s status=%d body=%r", host, status_code, body_text)
    return {"ok": True, "status_code": status_code, "detail": body_text}


def format_cfgfs(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/cfgfs/format_confirm -- the operator confirmation that lets
    cfg_fs_mount.c actually erase and reformat the `cfg` LittleFS partition
    after it detected (at boot) that auto-formatting would silently discard
    data and instead set its deferred format-confirmation-pending gate
    (cfg_fs_mount.c's s_format_confirmation_pending; see
    App/drivers/persist/cfg_fs_format_gate.h and GET
    /api/cfgfs/format_pending for the read side of that gate -- not wrapped
    here, since it needs no auth and dashboard_http_client.get_cfgfs_status()
    already surfaces the same partition state).

    ROUTE_TIER_ADMIN (the admin session http_auth.urlopen() carries) is the
    only gate; the AP-password HMAC challenge/response this route used to
    also require was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b) -- it
    used to be signed over the same "factory-reset" context as POST
    /api/factory_reset (cfg_fs_format_http.c's format_confirm_post_handler()
    deliberately reused rather than minting its own), which is now moot.

    DESTRUCTIVE: on success this erases every file cfg_fs holds and remounts
    an empty filesystem. The response body is plain text (not JSON, same
    departure from the rest of this module as sw_reset() above) -- reported
    back as ``{"ok": True, "status_code": ..., "detail": "<body text>"}`` on
    any 2xx. A non-2xx (e.g. the board's own 500 "format failed: ...") raises
    OtaHttpError with that text in ``.detail``, same as every other write
    call in this module.

    NOT YET VERIFIED AGAINST REAL HARDWARE by this module's own test suite
    -- request construction/response-parsing are unit-tested with mocked
    HTTP only; see test_ota_http_client.py.
    """
    req = urllib.request.Request(
        _url(host, "/api/cfgfs/format_confirm"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
        },
    )
    log.info("cfgfs format_confirm requested: host=%s", host)
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            status_code = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("cfgfs format_confirm refused: host=%s status=%s detail=%s",
                    host, status_code, detail)
        raise OtaHttpError(f"/api/cfgfs/format_confirm refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        log.warning("cfgfs format_confirm failed (unreachable): host=%s detail=%s", host, detail)
        raise OtaHttpError(f"/api/cfgfs/format_confirm unreachable: {detail}") from exc

    log.info("cfgfs format_confirm accepted: host=%s status=%d body=%r", host, status_code, body_text)
    return {"ok": True, "status_code": status_code, "detail": body_text}


def get_interlock(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/ota/interlock -- ROUTE_TIER_ADMIN (route_tier_table.h),
    ota_http_recovery.c's ota_interlock_get_handler()
    read of the board's own live OTA interlock state
    (ota_http_check_interlocks() / ota_interlock.c). Returns
    ``{"ok": true}`` when idle-and-clear, or
    ``{"ok": false, "reason": "<why>", "needs_ack": bool}`` otherwise.

    Plan doc section 6 rule 1: "Flash or OTA while ... GET
    /api/ota/interlock is not ok -- checked immediately before the call,
    not at run start." This is the client-side helper bench_test's
    cases_ota.py calls right before every OTA action (push/rollback) to
    honor that rule; no new firmware route was added for this -- the route
    has existed since ota_http_recovery.c's original landing.

    A non-2xx response (unexpected -- this route does not itself refuse
    with an HTTP error status, it reports the refusal IN the 200 body) or
    unparseable JSON both raise ``OtaHttpError``, same as every other
    reader in this module -- a caller must never treat "could not read
    the interlock" as "the interlock is ok".
    """
    req = urllib.request.Request(_url(host, "/api/ota/interlock"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/interlock refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/interlock unreachable: {detail}") from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/ota/interlock response was not valid JSON: {body_text!r}") from exc


def push_esp_image_unauthenticated(host: str, path: str,
                                    timeout: float = OTA_ESP_UPLOAD_TIMEOUT_S) -> OtaPushResult:
    """OT-E09: POST /api/ota/esp with NO admin session cookie at all --
    confirms the board refuses an update pushed with no credential rather
    than silently accepting one because some other check (interlock, size)
    happened to be satisfied. Deliberately bypasses ``_push_image()``'s
    ``http_auth.urlopen()`` admin-session seam entirely rather than routing
    through it, since a caller with that wrapper's environment credentials
    set would otherwise auto-login and defeat the case (see the comment
    below).

    A non-2xx response is reported the same way ``_push_image()`` does
    (`OtaPushResult(ok=False, ...)`) rather than raising, so callers can
    use the same result-object comparison as every other push case;
    ``OtaHttpError`` is still raised for a genuinely unparseable response
    or a transport failure, both of which also count as "refused" from a
    caller's point of view.
    """
    if not os.path.isfile(path):
        raise OtaHttpError(f"no such file: {path}")
    with open(path, "rb") as f:
        data = f.read()
    req = urllib.request.Request(
        _url(host, "/api/ota/esp"),
        data=data,
        method="POST",
        headers={"Content-Type": "application/octet-stream", "Content-Length": str(len(data))},
    )
    log.info("OTA push (no credential) starting: host=%s path=%s size=%d", host, path, len(data))
    try:
        # Deliberately NOT http_auth.urlopen: that wrapper auto-logs-in and
        # retries once on a 401 using KILNCTL_WEB_USERNAME/PASSWORD from the
        # environment -- exactly the credential this case exists to prove is
        # NOT presented. Using it here would silently authenticate an
        # "unauthenticated" push whenever those variables happen to be set,
        # defeating OT-E09 outright.
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status_code = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("OTA push (no credential) refused: host=%s status=%s detail=%s", host, status_code, detail)
        return OtaPushResult(ok=False, status_code=status_code or 0, body={"reason": detail})
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/esp unreachable: {detail}") from exc
    try:
        body = json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/ota/esp response was not valid JSON: {body_text!r}",
                            status_code, body_text) from exc
    return OtaPushResult(ok=bool(body.get("ok")), status_code=status_code, body=body)


def push_esp_image_with_session(host: str, path: str, session_cookie: str,
                                 timeout: float = OTA_ESP_UPLOAD_TIMEOUT_S) -> OtaPushResult:
    """OT-E10: POST /api/ota/esp authenticated by a ``kiln_sid`` web-auth
    session cookie -- the only auth this route has now that the AP-password
    HMAC challenge/response scheme was retired 2026-09-29 (WEB_AUTH_PLAN.md
    item 2b): ``kiln_http_register()``'s enforcement pre-handler requires a
    valid ADMIN session before this handler is ever reached. A non-admin
    (``user``-tier) session is expected to be refused by that same
    pre-handler (403) before ever reaching ota_http.c's own logic, which is
    exactly what OT-E10 exercises by calling this twice, once per tier.
    """
    if not os.path.isfile(path):
        raise OtaHttpError(f"no such file: {path}")
    with open(path, "rb") as f:
        data = f.read()
    req = urllib.request.Request(
        _url(host, "/api/ota/esp"),
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
            "Cookie": f"kiln_sid={session_cookie}",
        },
    )
    log.info("OTA push (session auth) starting: host=%s path=%s size=%d", host, path, len(data))
    try:
        # Deliberately NOT http_auth.urlopen: this case supplies its own
        # session cookie explicitly (admin vs. user tier) and must not have
        # it silently swapped out or retried against a *different* logged-in
        # identity by that wrapper's own auto-login-on-401 behaviour.
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status_code = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        log.warning("OTA push (session auth) refused: host=%s status=%s detail=%s", host, status_code, detail)
        return OtaPushResult(ok=False, status_code=status_code or 0, body={"reason": detail})
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/esp unreachable: {detail}") from exc
    try:
        body = json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/ota/esp response was not valid JSON: {body_text!r}",
                            status_code, body_text) from exc
    return OtaPushResult(ok=bool(body.get("ok")), status_code=status_code, body=body)
