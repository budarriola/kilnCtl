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
shape. get_partitions() now recognizes the recovery shape and raises
``RecoveryImageResponse`` (carrying ``running``/``running_offset``/
``next_update``) instead of the generic "malformed" ``PartitionHttpError`` --
deliberately NOT normalizing it into the main app's
``{"running", "partitions": [...]}`` shape with an empty ``partitions``
list, since a caller that only checks ``"partitions" in data`` (rather than
an explicit recovery flag) would then read an empty table as "board has NO
partitions" instead of "board didn't answer with a table at all", which is
a worse misdiagnosis than the one this fix replaces. Callers that need to
tell the two images apart catch ``RecoveryImageResponse`` explicitly (see
``mcp_server_flash._verify_flash_landed()`` and
``partition_table.read_chip_partition_table_from_http()``); callers that
don't care still get a raise, same as before this fix, via
``RecoveryImageResponse``'s ``PartitionHttpError`` base class.
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


class RecoveryImageResponse(PartitionHttpError):
    """Raised by get_partitions() in place of the generic "malformed
    response" PartitionHttpError when the response is recognizably the
    RECOVERY image's shape (KilnFW_recovery/main/recovery_http.c's
    partitions_get(): {"running", "running_offset", "next_update"}, no
    "partitions" array) rather than an actually broken one. A subclass of
    PartitionHttpError, not a sibling exception, so any existing
    ``except PartitionHttpError`` still catches it -- a caller that hasn't
    been updated to distinguish the two keeps its prior "proceed without
    this confirmation" behaviour unchanged, and only a caller that wants
    the more precise diagnosis needs to add an
    ``except RecoveryImageResponse`` before it.

    ``running``/``running_offset``/``next_update`` mirror the response
    body's own field names exactly (running_offset stays the hex string the
    firmware sent, e.g. "0x009000" -- this class does no numeric parsing)."""

    def __init__(self, running: str, running_offset: str, next_update: str, body_text: str = ""):
        super().__init__(
            f"GET /api/partitions: board is running the RECOVERY image "
            f"(running={running!r}, running_offset={running_offset!r}, "
            f"next_update={next_update!r}), not the main app"
        )
        self.running = running
        self.running_offset = running_offset
        self.next_update = next_update
        self.detail = body_text


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
    ``{"running": <label>, "partitions": [...]}`` (main app shape only).
    Raises RecoveryImageResponse (a PartitionHttpError subclass -- see its
    docstring) when the response is the recovery image's
    ``{"running", "running_offset", "next_update"}`` shape instead. Raises
    plain PartitionHttpError on any transport failure, non-2xx response,
    invalid JSON, or a response missing "running" entirely, missing
    "partitions" without also looking like the recovery shape, or any
    per-entry required field on the main-app shape -- loud failure rather
    than a partial or empty table read back as "MATCH: no partitions" would
    be a dangerously wrong reading of the board's real state."""
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
        # malformed one, but not one this function returns as data: raise
        # a distinguishable exception so a caller has to opt in to reading
        # a recovery-mode board's fields, rather than silently getting an
        # empty partitions table that reads as "board has no partitions".
        if "next_update" in data or "running_offset" in data:
            raise RecoveryImageResponse(
                running=data.get("running", "?"),
                running_offset=data.get("running_offset", "?"),
                next_update=data.get("next_update", "?"),
                body_text=body_text,
            )
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
    return data
