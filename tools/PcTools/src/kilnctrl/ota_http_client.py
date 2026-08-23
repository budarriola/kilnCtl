#!/usr/bin/env python3
"""ota_http_client.py -- pure HTTP client for the ESP's OTA update endpoints
(firmware/KilnFW/App/drivers/ota_http.c), used by mcp_server.py's ota_* tools.

Talks straight HTTP to /api/ota/{challenge,esp,pico} and GET
/api/ota/pico/status -- same "stdlib urllib.request, no framework" convention
gui.py already uses for this board's other HTTP surfaces (Wi-Fi settings
popup, Rules editor -- see gui.py's _wifi_default_host()/
_wifi_http_error_text()). Kept in its own module, separate from
mcp_server.py, specifically so the request-construction / HMAC-signing /
response-parsing logic here can be unit-tested with mocked HTTP responses
(tools/PcTools/tests/test_ota_http_client.py) with no real socket and no
live board required.

Wire contract source of truth: firmware/KilnFW/App/drivers/ota_http.h/.c and
firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 2 (auth) and section 3 (ESP
transfer)/section 9.5-era pico staging. Mirrored here, not re-derived:

  GET  /api/ota/challenge      -> 200 {"nonce": "<32 hex chars>"}  (16 raw bytes)
  key  = HMAC-SHA256(ap_password, "kilnctl-ota-v1")
  mac  = HMAC-SHA256(key, nonce_bytes || b"esp"  )   for /api/ota/esp
       = HMAC-SHA256(key, nonce_bytes || b"pico" )   for /api/ota/pico
  POST /api/ota/esp,  header X-Ota-Mac: <64 hex chars>, raw .bin body
       -> 200 {"ok":true,"bytes":N,"partition":"...","version":"..."}
  POST /api/ota/pico, same header/body shape, image is SaftyFW's raw .bin
       -> 202 {"ok":true,"status":"relay_started","bytes":N,"crc32":"0x..."}
  GET  /api/ota/pico/status    -> 200 {"phase":"...","percent":N,"last_error":"..."}
  GET  /api/ota/esp/status     -> 200 {"phase":"...","percent":N,"last_update":null|{...}}
  POST /api/ota/esp/rollback, header X-Ota-Mac: <64 hex chars> over context
       "esp-rollback" (NOT the same MAC as /api/ota/esp -- distinct context
       string), empty body -> 200 {"ok":true,"status":"rebooting",
       "version_before":"..."}. Explicit revert to the previous OTA image;
       reboots the board shortly after responding.
  POST /api/ota/esp/recovery_exit, header X-Ota-Mac: <64 hex chars> over
       context "recovery" (its own context, distinct from "esp"/
       "esp-rollback"/"pico"), empty body -> 200 {"ok":true,
       "status":"rebooting"}, or 403 "board is not in recovery mode" if the
       board is not currently in boot_guard.h's recovery mode. Reboots the
       board shortly after responding, same as rollback above.

Failure responses (400/403/409/500) are PLAIN TEXT
(httpd_resp_send_err()/httpd_resp_set_status()+httpd_resp_send()), not JSON --
so this client reads a non-2xx body as text and surfaces it verbatim in
OtaHttpError.detail rather than trying to json.loads() it.

CLOSED GAP (was open through 2026-08-18): there used to be no HTTP endpoint
exposing ota_http_get_esp_progress() (the ESP self-update's own progress
counter) or the persisted ota_record (App/drivers/ota_record.h's "last
update" NVS blob). `GET /api/ota/esp/status` (App/drivers/ota_http.c) now
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
import hmac
import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Optional

#: UPDATE_PROTOCOL.md section 2 step 2's literal KDF context string.
OTA_KDF_CONTEXT = b"kilnctl-ota-v1"

#: Short requests: challenge issue, pico status poll.
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
OTA_AP_DEFAULT_HOST = "192.168.4.1"


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


def derive_mac(ap_password: str, nonce: bytes, context: str) -> bytes:
    """HMAC-SHA256(HMAC-SHA256(ap_password, "kilnctl-ota-v1"), nonce || context)
    -- CommonFW/docs/UPDATE_PROTOCOL.md section 2 step 2, byte-for-byte what
    ota_http.c's ota_http_verify_request() recomputes server-side (see that
    function: "key = HMAC-SHA256(ap_password, ...)" is the ap_password as
    the HMAC KEY and the context string as the message -- this derivation is
    what keeps the literal Wi-Fi/AP password out of the value that's ever
    compared or sent). `context` must be exactly "esp", "pico", or
    "esp-rollback" (the last is its own context, not a reuse of "esp" -- see
    ota_http.h's doc comment on OTA_HTTP_CONTEXT_ESP_ROLLBACK for why a
    plain-update MAC must not double as a rollback authorization).
    """
    if context not in ("esp", "pico", "esp-rollback", "recovery"):
        raise ValueError(
            f"context must be 'esp', 'pico', 'esp-rollback', or 'recovery', got {context!r}")
    key = hmac.new(ap_password.encode("utf-8"), OTA_KDF_CONTEXT, hashlib.sha256).digest()
    msg = nonce + context.encode("ascii")
    return hmac.new(key, msg, hashlib.sha256).digest()


def get_challenge(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> bytes:
    """GET /api/ota/challenge -> the 16-byte nonce, decoded from hex.
    Single-use, 30 s expiry server-side (ota_auth.h) -- a caller must derive
    the MAC and POST within that window; a stale/reused nonce comes back as
    a 403 ("no valid challenge...") from the push call, not from here."""
    req = urllib.request.Request(_url(host, "/api/ota/challenge"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001 - normalized into OtaHttpError below
        status, detail = _http_error_detail(exc)
        raise OtaHttpError(f"challenge request failed: {detail}", status, detail) from exc

    try:
        obj = json.loads(body)
        nonce_hex = obj["nonce"]
    except Exception as exc:
        raise OtaHttpError(f"challenge response was not the expected JSON: {body!r}") from exc

    try:
        nonce = bytes.fromhex(nonce_hex)
    except (TypeError, ValueError) as exc:
        raise OtaHttpError(f"challenge nonce was not valid hex: {nonce_hex!r}") from exc
    if len(nonce) != 16:
        raise OtaHttpError(f"challenge nonce was {len(nonce)} bytes, expected 16")
    return nonce


@dataclass
class OtaPushResult:
    ok: bool
    status_code: int
    body: dict = field(default_factory=dict)


def _push_image(host: str, path: str, endpoint: str, context: str, ap_password: str,
                 timeout: float) -> OtaPushResult:
    """Shared body of push_esp_image()/push_pico_image(): validate the local
    file, fetch a fresh challenge, sign it, and POST the raw bytes with the
    X-Ota-Mac header -- the exact order ota_esp_post_handler()/
    ota_pico_post_handler() check in (header well-formed -> auth -> ...), so
    a malformed local request never reaches the board's interlock/mutex
    checks at all. Nothing here retries a partial write: any exception
    anywhere in this function propagates as-is, and a caller must treat that
    as "unknown whether the board received anything usable" -- re-uploading
    is a fresh, explicit action, never something this function does on its
    own behalf.
    """
    if not os.path.isfile(path):
        raise OtaHttpError(f"no such file: {path}")
    size = os.path.getsize(path)
    if size == 0:
        raise OtaHttpError(f"image file is empty: {path}")

    nonce = get_challenge(host)
    mac_hex = derive_mac(ap_password, nonce, context).hex()

    with open(path, "rb") as f:
        data = f.read()

    req = urllib.request.Request(
        _url(host, endpoint),
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
            "X-Ota-Mac": mac_hex,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status_code = resp.status
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise OtaHttpError(f"{endpoint} refused: HTTP {status_code}: {detail}", status_code,
                            detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"{endpoint} unreachable: {detail}") from exc

    try:
        body = json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"{endpoint} response was not valid JSON: {body_text!r}",
                            status_code, body_text) from exc

    return OtaPushResult(ok=bool(body.get("ok")), status_code=status_code, body=body)


def push_esp_image(host: str, path: str, ap_password: str,
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
    return _push_image(host, path, "/api/ota/esp", "esp", ap_password, timeout)


def push_pico_image(host: str, path: str, ap_password: str,
                     timeout: float = OTA_PICO_STAGE_TIMEOUT_S) -> OtaPushResult:
    """POST /api/ota/pico -- streams the raw SaftyFW .bin into the `pico_img`
    staging partition at Wi-Fi speed, then returns as soon as staging
    finishes (202 Accepted,
    {"ok":true,"status":"relay_started","bytes":N,"crc32":"0x..."}) -- it does
    NOT wait for the ~35s+ relay over the isolated UART link to the RP2040 to
    finish. A 202 here means "upload accepted and the relay task started",
    never "the safety processor is now running the new image" -- poll
    get_pico_status() afterward for the actual relay outcome.
    """
    return _push_image(host, path, "/api/ota/pico", "pico", ap_password, timeout)


def get_pico_status(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/ota/pico/status -- unauthenticated poll-back for the
    background relay task's progress (ota_pico_relay.c). Returns
    {"phase": "...", "percent": 0-100, "last_error": "..."} straight from the
    board; phase is one of ota_pico_relay_phase_str()'s strings (idle,
    begin, erasing, sending, retransmit, finishing, done, failed)."""
    req = urllib.request.Request(_url(host, "/api/ota/pico/status"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise OtaHttpError(f"pico status request failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"pico status response was not valid JSON: {body_text!r}") from exc


def get_esp_status(host: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """GET /api/ota/esp/status -- unauthenticated poll-back for the ESP's own
    self-update transfer (ota_http_get_esp_progress()) plus the persisted
    "last update" NVS record (ota_record.h). Returns
    {"phase": "...", "percent": 0-100, "last_update": null | {...}} straight
    from the board; phase is one of "idle"/"verifying"/"writing"/
    "finalizing"/"done"/"failed" (ota_http.c's esp_phase_str()). last_update
    is None (JSON null) until the first ESP update has ever run this NVS
    lifetime; once one has, it is a dict with processor/version_before/
    version_after/success/reason/uptime_s, mirroring ota_record_t."""
    req = urllib.request.Request(_url(host, "/api/ota/esp/status"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise OtaHttpError(f"esp status request failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"esp status response was not valid JSON: {body_text!r}") from exc


def rollback_esp(host: str, ap_password: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/rollback -- explicit "revert to the previous image
    right now" (App/drivers/ota_http.c's ota_esp_rollback_post_handler()).
    Unlike push_esp_image()/push_pico_image(), there is no file to send --
    the body is empty, only the challenge/MAC dance and the X-Ota-Mac header
    are needed.

    Refused the same way an update push is: wrong/missing auth (403), an
    unmet interlock (409, kiln not idle/cool or similar -- see
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

    `ap_password`: same AP-password-derived HMAC scheme as push_esp_image()/
    push_pico_image() -- see derive_mac()'s doc comment. Uses the
    "esp-rollback" context, a distinct signature from a plain "esp" update
    MAC (ota_http.h's OTA_HTTP_CONTEXT_ESP_ROLLBACK doc comment).
    """
    nonce = get_challenge(host, timeout)
    mac_hex = derive_mac(ap_password, nonce, "esp-rollback").hex()

    req = urllib.request.Request(
        _url(host, "/api/ota/esp/rollback"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
            "X-Ota-Mac": mac_hex,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/esp/rollback refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/esp/rollback unreachable: {detail}") from exc

    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(f"/api/ota/esp/rollback response was not valid JSON: {body_text!r}") from exc


def recovery_exit_esp(host: str, ap_password: str, timeout: float = OTA_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/recovery_exit -- ask the board to reboot right now
    to exit boot_guard.h's recovery mode, rather than waiting for it to
    self-clear (App/drivers/ota_http.c's ota_recovery_exit_post_handler()).

    Same challenge/MAC dance as rollback_esp(), signed over its own
    "recovery" context (ota_http.h's OTA_HTTP_CONTEXT_RECOVERY_EXIT) -- NOT
    interchangeable with an "esp"/"pico"/"esp-rollback" MAC. Refused (403)
    the same way a wrong password is if auth fails, and ALSO refused (403,
    "board is not in recovery mode") if the board is not currently in
    recovery mode -- that check runs AFTER auth on the board side
    specifically so a caller who never proves they hold the AP password
    cannot use this call to probe whether the board is in recovery mode (see
    ota_recovery_exit_post_handler()'s doc comment for the full reasoning).

    On success (200), the board is already rebooting from a short-lived
    background task -- this call returns as soon as the response arrives,
    it does NOT wait for the reboot to finish. Returns the parsed JSON body,
    {"ok": true, "status": "rebooting"}.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment); no ESP32-S3 was available in
    this environment to actually trigger recovery mode and exit it.
    """
    nonce = get_challenge(host, timeout)
    mac_hex = derive_mac(ap_password, nonce, "recovery").hex()

    req = urllib.request.Request(
        _url(host, "/api/ota/esp/recovery_exit"),
        data=b"",
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": "0",
            "X-Ota-Mac": mac_hex,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/esp/recovery_exit refused: HTTP {status_code}: {detail}",
                            status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise OtaHttpError(f"/api/ota/esp/recovery_exit unreachable: {detail}") from exc

    try:
        return json.loads(body_text)
    except Exception as exc:
        raise OtaHttpError(
            f"/api/ota/esp/recovery_exit response was not valid JSON: {body_text!r}") from exc
