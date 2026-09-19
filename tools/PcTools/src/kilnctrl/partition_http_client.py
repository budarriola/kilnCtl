#!/usr/bin/env python3
"""partition_http_client.py -- thin HTTP client for GET /api/partitions
(firmware/KilnFW/App/drivers/http/partition_info_http.c).

FLASH_BUDGET.md section 8 item 3 originally read the on-chip partition
table over JTAG at flash offset 0x8000 (partition_table.py's
``read_chip_partition_table_bytes``, via ``debug_probe.read_memory``). That
does not work: 0x8000 is a FLASH offset, and OpenOCD's ``read_memory``
targets the CPU's memory-mapped address space, not raw flash -- confirmed
against the real board (``failed to read 4096 B from esp flash at 0x8000``
/ ``DEPRECATED! use 'read_memory' not 'mem2array'`` / ``failed to read
memory``). See ``partition_table.py``'s module docstring and
``read_chip_partition_table_bytes``'s docstring for the full writeup; that
function is kept for its historical/pure-parsing value but is no longer the
default path anything calls.

This module is the replacement data source: GET /api/partitions asks the
RUNNING FIRMWARE what partition table it is actually using (via ESP-IDF's
``esp_partition_find``/``esp_partition_next`` iterator, from inside the
app -- no JTAG, no core halt). Same "stdlib urllib.request, no framework"
convention as ``dashboard_http_client.py``/``ota_http_client.py``, and unit
tested the same way (mocked urllib responses, no real socket, no live
board; see ``tools/PcTools/tests/test_partition_http_client.py``).

Response shape (partition_info_http.c's api_partitions_get_handler()):

    {"running": "kiln_app_a",
     "partitions": [
       {"label": "nvs", "type": 1, "subtype": 2, "offset": 36864,
        "size": 24576, "encrypted": false},
       ...
     ]}

The recovery image (firmware/KilnFW_recovery/main/recovery_http.c's
partitions_get()) answers the same route with a different, size-budget-
driven shape instead -- it is a separate, much smaller build
(check_recovery_image_size.ps1 grades it) that never links
partition_info_http.c:

    {"running": "recovery", "running_offset": "0x009000", "next_update": "app"}

docs/audits/web_code_duplication_drift_2026-09-18.md section 2.3 recorded
that get_partitions() used to reject this second shape outright ("response
missing 'running'/'partitions'"), which meant a board that came up in
recovery mode -- something CLAUDE.md records happening from ordinary
flashing -- was misdiagnosed as returning a malformed response instead of
reporting the much more useful fact that it is running the recovery image.
Per that audit, the fix belongs on this client (recognizing the recovery
shape) rather than growing the recovery image to match the main app's
shape. get_partitions() now recognizes both: a recovery-shaped response is
normalized to also carry ``"partitions": []`` and ``"is_recovery_shape":
True`` so every caller can keep reading ``data["running"]`` unconditionally
-- callers that care about telling the two images apart check
``is_recovery_shape``.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request

from . import http_auth
from typing import Optional

PARTITION_HTTP_TIMEOUT_S = 5.0

#: Same fallback-AP address every other *_http_client.py in this directory
#: uses (ota_http_client.OTA_AP_DEFAULT_HOST, dashboard_http_client.
#: DASHBOARD_AP_DEFAULT_HOST) -- the board's own softAP address, reachable
#: even with no home Wi-Fi configured.
PARTITION_AP_DEFAULT_HOST = "192.168.4.1"

_REQUIRED_KEYS = ("label", "type", "subtype", "offset", "size", "encrypted")


class PartitionHttpError(RuntimeError):
    """Raised on transport failure, a non-2xx response, or a response body
    that isn't valid/complete JSON. Mirrors DashboardHttpError/OtaHttpError's
    shape (message, optional HTTP status, optional detail text)."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace").strip()
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def get_partitions(host: str, timeout: float = PARTITION_HTTP_TIMEOUT_S) -> dict:
    """GET /api/partitions and return the decoded JSON object
    ``{"running": <label>, "partitions": [...]}`` (main app), or the
    recovery image's ``{"running": <label>, "running_offset": <hex str>,
    "next_update": <label>}`` normalized to also carry ``"partitions": []``
    and ``"is_recovery_shape": True`` -- see this module's docstring for why
    both shapes are legitimate rather than one being malformed. Raises
    PartitionHttpError on any transport failure, non-2xx response, invalid
    JSON, or a response missing "running" entirely, missing "partitions"
    without also looking like the recovery shape, or any per-entry required
    field on the main-app shape -- loud failure rather than a partial or
    empty table read back as "MATCH: no partitions" would be a dangerously
    wrong reading of the board's real state."""
    req = urllib.request.Request(_url(host, "/api/partitions"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise PartitionHttpError(f"GET /api/partitions failed: {detail}", status, detail) from exc

    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise PartitionHttpError(f"GET /api/partitions response was not valid JSON: {body_text!r}") from exc

    if not isinstance(data, dict) or "running" not in data:
        raise PartitionHttpError(
            f"GET /api/partitions response missing 'running'/'partitions': {body_text!r}"
        )

    if "partitions" not in data:
        # Not the main app's shape. The recovery image
        # (KilnFW_recovery/main/recovery_http.c's partitions_get()) answers
        # this same route with {"running", "running_offset", "next_update"}
        # instead -- a valid, deliberately smaller response, not a
        # malformed one. Normalize it so every caller can keep reading
        # data["running"] unconditionally.
        if "next_update" in data or "running_offset" in data:
            normalized = dict(data)
            normalized["partitions"] = []
            normalized["is_recovery_shape"] = True
            return normalized
        raise PartitionHttpError(
            f"GET /api/partitions response missing 'running'/'partitions': {body_text!r}"
        )

    if not isinstance(data["partitions"], list):
        raise PartitionHttpError(
            f"GET /api/partitions 'partitions' was not a list: {body_text!r}"
        )
    for i, entry in enumerate(data["partitions"]):
        if not isinstance(entry, dict):
            raise PartitionHttpError(f"GET /api/partitions entry {i} was not an object: {entry!r}")
        missing = [k for k in _REQUIRED_KEYS if k not in entry]
        if missing:
            raise PartitionHttpError(
                f"GET /api/partitions entry {i} ({entry.get('label', '?')!r}) missing {missing}"
            )
    data = dict(data)
    data["is_recovery_shape"] = False
    return data
