"""recovery_ota_auth_client.py -- the AP-password X-Ota-Mac signer, kept alive
ONLY for firmware/KilnFW_recovery/ (the standalone recovery-image firmware),
after the main app retired the whole scheme 2026-09-29
(WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open when login off").

Why this still exists at all: firmware/KilnFW_recovery/ is a genuinely
separate, standalone firmware image (its own IDF project, its own
recovery_http.c, its own ota_auth.c mirror of the scheme the main app just
retired -- see recovery_ota_auth_mirror_drift_check.py) that a board runs
ONLY while it is stuck in boot_guard.h's recovery mode, unable to reach the
main app at all. That image was never touched by the 2026-09-29 retirement
(explicit scope boundary: "keep firmware/KilnFW_recovery/ and its ota_auth.c
mirror completely untouched") and its own mutating routes -- POST
/api/ota/esp, POST /api/ota/esp/boot_guard_reset, POST /api/sw_reset -- still
require a signed X-Ota-Mac header derived from the board's AP Wi-Fi password,
fetched fresh from GET /api/ota/challenge, exactly as the main app's nine
routes used to. `kilnctrl.ota_http_client` no longer knows how to sign
anything; calling its (now-unauthenticated) helpers against a board that has
actually fallen back to the recovery image will just get a 401/403 from that
image's own ota_auth.c, since that image was never told the scheme went away
on the other side.

This module is the narrow, still-needed replacement for exactly that one
case: it never talks to the main app, and it must never be reached for by a
caller that already knows the board is answering normally (use
`kilnctrl.ota_http_client` and an admin web session for that).

The AP password is read once, by the caller, from KILNCTL_AP_PASSWORD --
this module never reads environment variables itself and never logs or
echoes the password or the derived HMAC key. Only the resulting nonce/MAC hex
strings are logged.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import logging
import urllib.error
import urllib.request
from typing import Optional

log = logging.getLogger("kilnctrl.recovery_ota_auth_client")

#: Mirrors ota_http.h's (pre-retirement) and recovery_http.c's (current, live)
#: literal KDF context string -- UPDATE_PROTOCOL.md section 2 step 2.
OTA_KDF_CONTEXT = b"kilnctl-ota-v1"

#: Contexts the recovery image's mutating routes sign under -- see
#: recovery_http.c's ROUTE_CONTEXT_MARKERS-equivalent call sites
#: (recovery_ota_auth_mirror_drift_check.py mirrors these same
#: strings). Deliberately a small, closed set: a caller asking for any other
#: context is almost certainly confusing this module with the retired
#: main-app scheme, which had six.
_VALID_CONTEXTS = ("esp", "boot-guard-reset", "sw-reset", "recovery-exit", "wifi-reset",
                   "pico-upload", "pico-abort")

DEFAULT_TIMEOUT_S = 10.0


class RecoveryOtaAuthError(RuntimeError):
    """A challenge fetch or signed POST against the recovery image failed."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def derive_mac(ap_password: str, nonce: bytes, context: str) -> bytes:
    """HMAC-SHA256(HMAC-SHA256(ap_password, "kilnctl-ota-v1"), nonce || context).

    Byte-for-byte the same derivation the main app used before 2026-09-29
    (see that history in git blame of the old kilnctrl.ota_http_client
    module) and the one recovery_http.c's ota_auth.c mirror still implements
    today -- this function must never drift from that mirror, or a correctly
    signed request from here will be rejected by a genuinely healthy
    recovery image.

    `context` must be one of _VALID_CONTEXTS. Anything else is almost always
    a copy-paste of a retired main-app context string ("esp-rollback",
    "pico", "pico-rollback", "recovery", "factory-reset" all existed there
    but have no recovery-image counterpart) and is refused rather than
    silently producing a MAC nothing will ever accept.
    """
    if context not in _VALID_CONTEXTS:
        raise ValueError(
            f"context must be one of {_VALID_CONTEXTS!r} (recovery-image routes only), got {context!r}")
    key = hmac.new(ap_password.encode("utf-8"), OTA_KDF_CONTEXT, hashlib.sha256).digest()
    msg = nonce + context.encode("ascii")
    return hmac.new(key, msg, hashlib.sha256).digest()


def get_challenge(host: str, timeout: float = DEFAULT_TIMEOUT_S) -> bytes:
    """GET /api/ota/challenge against a board running the recovery image --
    returns the 16-byte nonce, decoded from hex. Unauthenticated, same as the
    main app's route used to be before it was deleted outright 2026-09-29;
    the recovery image's copy of this route is untouched and still open."""
    req = urllib.request.Request(_url(host, "/api/ota/challenge"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace") if exc.fp else ""
        raise RecoveryOtaAuthError(
            f"recovery-image challenge request failed: HTTP {exc.code}", exc.code, detail) from exc
    except urllib.error.URLError as exc:
        raise RecoveryOtaAuthError(f"recovery-image challenge request unreachable: {exc}") from exc
    try:
        data = json.loads(body.decode("utf-8"))
        nonce_hex = data["nonce"]
        nonce = bytes.fromhex(nonce_hex)
    except (ValueError, KeyError, UnicodeDecodeError) as exc:
        raise RecoveryOtaAuthError(
            f"recovery-image challenge response was not the expected JSON: {body!r}") from exc
    if len(nonce) != 16:
        raise RecoveryOtaAuthError(f"recovery-image challenge nonce was {len(nonce)} bytes, expected 16")
    return nonce


def _url(host: str, path: str) -> str:
    host = host.rstrip("/")
    if not host.startswith(("http://", "https://")):
        host = f"http://{host}"
    return f"{host}{path}"


def signed_post(host: str, path: str, context: str, ap_password: str, *,
                 data: bytes = b"", timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """Fetches a fresh challenge from the recovery image, signs it for
    `context`, and POSTs `data` (default empty body) with the resulting
    X-Ota-Mac header to `path` on that same host. Returns
    ``{"status": int, "text": str}`` -- the recovery image's three mutating
    routes answer a success with a PLAIN TEXT body
    ("ok, rebooting into new application image" for /api/ota/esp,
    "boot_guard counter cleared" for /api/ota/esp/boot_guard_reset,
    "resetting" for /api/sw_reset -- recovery_http.c:377, :443, :454), never
    JSON, so this never attempts to parse it as JSON.

    Every mutating recovery-image route needs its own fresh nonce -- the
    same one-shot-nonce contract the main app used to enforce, mirrored in
    recovery_http.c's ota_auth.c copy -- so this always calls get_challenge()
    itself rather than accepting a caller-supplied nonce.
    """
    nonce = get_challenge(host, timeout)
    mac_hex = derive_mac(ap_password, nonce, context).hex()
    req = urllib.request.Request(
        _url(host, path),
        data=data,
        method="POST",
        headers={"X-Ota-Mac": mac_hex, "Content-Type": "application/octet-stream"},
    )
    log.info("recovery-image signed POST: host=%s path=%s context=%s", host, path, context)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace") if exc.fp else ""
        log.warning("recovery-image signed POST refused: host=%s path=%s status=%s detail=%s",
                    host, path, exc.code, detail)
        raise RecoveryOtaAuthError(
            f"{path} refused by recovery image: HTTP {exc.code}: {detail}", exc.code, detail) from exc
    except urllib.error.URLError as exc:
        raise RecoveryOtaAuthError(f"{path} unreachable on recovery image: {exc}") from exc
    text = body.decode("utf-8", "replace")
    return {"status": status, "text": text}


def recovery_push_esp_image(host: str, image_bytes: bytes, ap_password: str,
                             timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/ota/esp against the RECOVERY image (recovery_http.c's
    ota_esp_post()) -- pushes a new ESP application image while the board is
    stuck in recovery mode and cannot reach the main app at all. Context
    "esp", same string the recovery image's own ota_esp_post() call site
    signs with (recovery_ota_auth_mirror_drift_check.py's ROUTE_CONTEXT_
    MARKERS pins this)."""
    return signed_post(host, "/api/ota/esp", "esp", ap_password, data=image_bytes, timeout=timeout)


def recovery_boot_guard_reset(host: str, ap_password: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/ota/esp/boot_guard_reset against the RECOVERY image
    (recovery_http.c's boot_guard_reset_post()) -- context "boot-guard-reset".
    Distinct from kilnctrl.ota_http_client.boot_guard_reset_esp(), which
    targets the MAIN APP's (ROUTE_TIER_ADMIN, no MAC) copy of this route and
    only works once the board is running normally again."""
    return signed_post(host, "/api/ota/esp/boot_guard_reset", "boot-guard-reset", ap_password,
                        timeout=timeout)


def recovery_sw_reset(host: str, ap_password: str, timeout: float = DEFAULT_TIMEOUT_S) -> dict:
    """POST /api/sw_reset against the RECOVERY image (recovery_http.c's
    sw_reset_post()) -- context "sw-reset". Distinct from
    kilnctrl.ota_http_client.sw_reset(), which targets the MAIN APP's
    (ROUTE_TIER_ADMIN, no MAC) copy."""
    return signed_post(host, "/api/sw_reset", "sw-reset", ap_password, timeout=timeout)
