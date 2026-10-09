#!/usr/bin/env python3
"""nvs_keys_http_client.py -- pure HTTP client for GET /api/nvs/keys
(firmware/KilnFW/App/drivers/http/diagnostics_http.c's
nvs_keys_get_handler()), a read-only diagnostic added 2026-09-21 to verify
on the bench that 1319e051's esp_wifi_restore() actually empties the
driver's own `nvs.net80211` namespace in the default `nvs` partition after
factory_reset(scope=wifi) -- see
docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md.

Same "stdlib urllib.request, no framework" convention as
readiness_http_client.py, in its own module so this READ-ONLY path stays
unit-tested against mocked HTTP responses
(tools/PcTools/tests/test_nvs_keys_http_client.py), no real socket and no
live board. This module never writes anything, and it never asks the board
for the `kiln_auth` namespace's keys -- the board itself refuses that
namespace with 403 (nvs_keys_get_handler()'s own guard), and this client
refuses it too before ever making the request, so a caller doesn't have to
learn the board's answer to find out.

Response shape: ``{"partition","namespace","keys":[{"key","type"}, ...]}``
where ``type`` is one of nvs_type_name()'s strings ("u8".."i64", "str",
"blob", "unknown"). Never a value, never a blob body -- key names and types
only."""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth

NVS_KEYS_HTTP_TIMEOUT_S = 8.0

_API_PATH = "/api/nvs/keys"

# Mirrors nvs_keys_get_handler()'s own hard refusal (403) -- kept here too so
# a caller gets an immediate, local error instead of a round trip to learn
# the same thing the board already enforces.
FORBIDDEN_NAMESPACE = "kiln_auth"


class NvsKeysHttpError(Exception):
    """Any transport or protocol failure talking to GET /api/nvs/keys --
    unreachable host, non-2xx, or a response shape this client does not
    understand."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, partition: str, namespace: str) -> str:
    query = urllib.parse.urlencode({"partition": partition, "namespace": namespace})
    return f"http://{host}{_API_PATH}?{query}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace")
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def get_nvs_keys(host: str, partition: str, namespace: str,
                  timeout: float = NVS_KEYS_HTTP_TIMEOUT_S) -> dict:
    """GET /api/nvs/keys?partition=<partition>&namespace=<namespace>.
    Returns the decoded JSON body: ``{"partition","namespace","keys":
    [{"key","type"}, ...]}``.

    Raises NvsKeysHttpError immediately, with no HTTP request made, for
    ``namespace == "kiln_auth"`` -- the board refuses this namespace too
    (403), but there is no reason to make the round trip to learn that.
    Also raises NvsKeysHttpError for a transport failure, non-2xx response,
    a non-JSON body, or a body missing the top-level ``keys`` list."""
    if namespace == FORBIDDEN_NAMESPACE:
        raise NvsKeysHttpError(
            f"namespace {FORBIDDEN_NAMESPACE!r} is never listed by this route "
            "(the board refuses it with 403; refusing locally instead of making the request)")
    req = urllib.request.Request(_url(host, partition, namespace), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise NvsKeysHttpError(f"GET {_API_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise NvsKeysHttpError(
            f"GET {_API_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or not isinstance(data.get("keys"), list):
        raise NvsKeysHttpError(f"GET {_API_PATH} response has no keys list: {body_text!r}")
    return data
