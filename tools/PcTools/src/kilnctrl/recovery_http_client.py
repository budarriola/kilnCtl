"""recovery_http_client.py -- unauthenticated, READ-ONLY GETs against the
standalone recovery image (firmware/KilnFW_recovery/main/recovery_http.c):
GET /api/recovery/status and GET /api/recovery/pico/status.

Plain urllib, deliberately NOT http_auth: the recovery image has no web-auth
sessions, and the whole image is unauthenticated (owner decision
2026-10-02; the LCD-passphrase SoftAP is the only access control). Mutating
routes live in recovery_post_client.py. Never used against a
board running the normal application (its /api/recovery/* routes do not
exist there and answer 404, which callers use as evidence that the recovery
image is gone).
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Optional

TIMEOUT_S = 6.0

#: recovery_pico.c recovery_pico_phase_name() values after which the relay
#: has stopped and the status will not change on its own. "outcome_unknown"
#: is terminal but is NOT a success (END was sent, the result was lost).
PICO_TERMINAL_PHASES = ("done", "failed", "aborted", "outcome_unknown")


class RecoveryHttpError(Exception):
    def __init__(self, message: str, status: Optional[int] = None):
        super().__init__(message)
        self.status = status


def _get_json(host: str, path: str, timeout: float) -> dict:
    url = f"http://{host}{path}"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method="GET"), timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        raise RecoveryHttpError(f"GET {path} answered HTTP {exc.code}", exc.code) from exc
    except (urllib.error.URLError, OSError) as exc:
        raise RecoveryHttpError(f"GET {path} unreachable: {exc}") from exc
    try:
        data = json.loads(text)
    except ValueError as exc:
        raise RecoveryHttpError(f"GET {path} body was not JSON: {text[:120]!r}") from exc
    if not isinstance(data, dict):
        raise RecoveryHttpError(f"GET {path} body was not a JSON object")
    return data


def get_status(host: str, timeout: float = TIMEOUT_S) -> dict:
    """GET /api/recovery/status. Must carry ``running == "recovery"`` or the
    caller is not talking to the recovery image."""
    return _get_json(host, "/api/recovery/status", timeout)


def get_pico_status(host: str, timeout: float = TIMEOUT_S) -> dict:
    """GET /api/recovery/pico/status. Note: while the relay is busy, polling
    this route is the relay's 'the operator is still watching' signal."""
    return _get_json(host, "/api/recovery/pico/status", timeout)
