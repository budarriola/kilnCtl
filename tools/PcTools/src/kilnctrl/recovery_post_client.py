"""recovery_post_client.py -- the mutating POSTs against the standalone recovery
image (firmware/KilnFW_recovery/main/recovery_http.c).

Since owner decision 2026-10-02 the recovery image is UNAUTHENTICATED: no
password, no key, no challenge, no signature header. Its only access control is
physical -- a WPA2 SoftAP whose random per-boot passphrase is shown on the
board's LCD and nowhere else (docs/RECOVERY_IMAGE_PLAN.md). The PC must already
be associated with that AP; nothing here joins it, and no credential is read or
needed by this module. (This file used to be recovery_ota_auth_client.py, the
HMAC signer for the retired challenge-response scheme.)

It must never be reached for by a caller that already knows the board is
answering normally (use `kilnctrl.ota_http_client` and an admin web session for
that).
"""
from __future__ import annotations

import logging
import urllib.error
import urllib.request
from typing import Optional

log = logging.getLogger("kilnctrl.recovery_post_client")

DEFAULT_TIMEOUT_S = 10.0

#: recovery_http.c's pico-upload query: "crc=xxxxxxxx&slot=A" is 19 chars.
QUERY_MAX = 95


class RecoveryPostError(RuntimeError):
    """A POST against the recovery image failed."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = "",
                 stage: str = "post"):
        super().__init__(message)
        self.status = status
        self.detail = detail
        #: Always "post". With ``status is None`` (transport failure, timeout,
        #: connection reset) the board MAY have received and acted on the
        #: request, and the caller must treat the outcome as unknown.
        self.stage = stage


def _check_query(query: str) -> None:
    """Refuse anything that could not be sent verbatim in a request line."""
    if query.startswith("?") or "#" in query:
        raise ValueError(f"query must be the text after '?' with no fragment, got {query!r}")
    if not all(33 <= ord(c) < 127 for c in query):
        raise ValueError(f"query must be printable ASCII with no spaces (percent-encode it first), got {query!r}")
    if len(query) > QUERY_MAX:
        raise ValueError(f"query is {len(query)} chars, the recovery image accepts at most {QUERY_MAX}")


def _url(host: str, path: str) -> str:
    host = host.rstrip("/")
    if not host.startswith(("http://", "https://")):
        host = f"http://{host}"
    return f"{host}{path}"


def post(host: str, path: str, *, data: bytes = b"", query: str = "",
         timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POSTs `data` (default empty body) to `path` on the recovery image.
    Returns ``{"status": int, "text": str}`` -- the image's mutating routes
    answer a success with a PLAIN TEXT body ("ok, rebooting into new
    application image" for /api/ota/esp, "boot_guard counter cleared" for
    /api/ota/esp/boot_guard_reset, "resetting" for /api/sw_reset), never JSON,
    so this never attempts to parse it as JSON.

    `query` (text after "?", already percent-encoded, default none) is
    appended as ``path?query``; `path` itself must not contain a "?".
    """
    if "?" in path:
        raise ValueError("pass the query via query=, not inside path")
    _check_query(query)
    req = urllib.request.Request(
        _url(host, path + ("?" + query if query else "")),
        data=data,
        method="POST",
        headers={"Content-Type": "application/octet-stream"},
    )
    log.info("recovery-image POST: host=%s path=%s", host, path)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace") if exc.fp else ""
        log.warning("recovery-image POST refused: host=%s path=%s status=%s detail=%s",
                    host, path, exc.code, detail)
        raise RecoveryPostError(
            f"{path} refused by recovery image: HTTP {exc.code}: {detail}", exc.code, detail) from exc
    except (urllib.error.URLError, OSError) as exc:
        # OSError: urllib raises a response-read timeout or a reset raw, not
        # as URLError -- by then the board may already have acted.
        raise RecoveryPostError(f"{path} transport failure on recovery image (outcome unknown): {exc}") from exc
    return {"status": status, "text": body.decode("utf-8", "replace")}


def recovery_push_esp_image(host: str, image_bytes: bytes, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/ota/esp against the RECOVERY image (ota_esp_post()) -- pushes
    a new ESP application image while the board is stuck in recovery mode."""
    return post(host, "/api/ota/esp", data=image_bytes, timeout=timeout)


def recovery_boot_guard_reset(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/boot_guard_reset against the RECOVERY image.
    Distinct from kilnctrl.ota_http_client.boot_guard_reset_esp(), which
    targets the MAIN APP's copy of this route."""
    return post(host, "/api/ota/esp/boot_guard_reset", timeout=timeout)


def recovery_sw_reset(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/sw_reset against the RECOVERY image. Distinct from
    kilnctrl.ota_http_client.sw_reset(), which targets the MAIN APP's copy."""
    return post(host, "/api/sw_reset", timeout=timeout)


def recovery_exit(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/recovery/exit (recovery_exit_post())."""
    return post(host, "/api/recovery/exit", timeout=timeout)


def recovery_wifi_reset(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/recovery/wifi_reset (wifi_reset_post())."""
    return post(host, "/api/recovery/wifi_reset", timeout=timeout)


def pico_upload_query(crc32: int, slot: Optional[str] = None) -> str:
    """The exact query string for POST /api/recovery/pico/upload:
    ``crc=<8 lowercase hex>[&slot=A|B]`` (parse_pico_query() in
    recovery_http.c; crc is required and must be exactly 8 hex digits)."""
    if not 0 <= crc32 <= 0xFFFFFFFF:
        raise ValueError(f"crc32 out of range: {crc32!r}")
    q = f"crc={crc32:08x}"
    if slot is not None:
        if slot not in ("A", "B"):
            raise ValueError(f"slot must be 'A', 'B' or None (auto), got {slot!r}")
        q += f"&slot={slot}"
    return q


def recovery_pico_upload(host: str, image_bytes: bytes, crc32: int,
                         slot: Optional[str] = None, timeout: float = 60.0) -> dict:
    """POST /api/recovery/pico/upload?crc=..[&slot=..] (pico_upload_post()).
    A 202 JSON body (``{"started":true,"image_slot":"A"}``) means the relay
    STARTED, nothing more; the outcome is only ever in
    GET /api/recovery/pico/status."""
    return post(host, "/api/recovery/pico/upload", data=image_bytes,
                query=pico_upload_query(crc32, slot), timeout=timeout)


def recovery_pico_abort(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/recovery/pico/abort (pico_abort_post()). Asks the Pico relay
    to stop; the board answers the plain text "abort requested" (also when the
    relay is idle) and the real result is only ever in
    GET /api/recovery/pico/status (phase "aborted")."""
    return post(host, "/api/recovery/pico/abort", timeout=timeout)


def recovery_apply_staged(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/recovery/apply_staged (apply_staged_post()). A 202 JSON body
    (``{"started":true,"image_length":N}``) means the apply task STARTED,
    nothing more; progress and outcome are only ever in
    GET /api/recovery/apply_status. 409 carries the reason as plain text
    ("nothing is staged", "Pico update in progress", ...)."""
    return post(host, "/api/recovery/apply_staged", timeout=timeout)
