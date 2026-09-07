#!/usr/bin/env python3
"""dashboard_http_client.py -- thin HTTP client for GET /api/status's heap
sub-objects (firmware/KilnFW/App/drivers/http/dashboard_http.c:386-397/837-843).

DRAM_PSRAM_PLAN.md Phase 0 (4.1): the firmware-side data has existed since
before this plan was written -- dashboard_get_status() already populates
heap_internal/heap_spiram (and, as of this module, heap_dma) and
dashboard_status_handler() already serialises all three into GET
/api/status's JSON. What was missing was a tool anywhere in this tree that
parses those three keys; nothing did, so the data was reachable by hand
(curl, a browser) but not from any measurement this plan's later phases
depend on. This module is exactly that thin a wrapper: one GET, one JSON
parse, three known keys pulled out -- same "stdlib urllib.request, no
framework" convention as ota_http_client.py/zones_http_client.py, and unit
tested the same way (mocked urllib responses, no real socket, no live board;
see tools/PcTools/tests/test_dashboard_http_client.py).

Each of heap_internal/heap_spiram/heap_dma carries free/largest_free_block/
min_free/total, all in bytes, straight from heap_caps_get_*() on the target
-- see dashboard_http.h's field comments for what MALLOC_CAP_INTERNAL vs.
MALLOC_CAP_SPIRAM vs. MALLOC_CAP_DMA each mean and why heap_dma is a strict
subset of heap_internal rather than new information.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Optional

DASHBOARD_HTTP_TIMEOUT_S = 5.0

# Same default as ota_http_client.OTA_AP_DEFAULT_HOST -- the board's own
# softAP address, reachable even with no home Wi-Fi configured. Not imported
# from there to avoid a cross-module dependency for one string literal; kept
# identical on purpose (see this module's own test for a same-value check).
DASHBOARD_AP_DEFAULT_HOST = "192.168.4.1"

_HEAP_KEYS = ("heap_internal", "heap_spiram", "heap_dma")


class DashboardHttpError(RuntimeError):
    """Raised on transport failure, a non-2xx response, or a response body
    that isn't valid JSON. Mirrors ZonesHttpError/OtaHttpError's shape
    (message, optional HTTP status, optional detail text) for the same
    reason those exist: an MCP tool wrapping this needs to report a
    connectivity failure differently from a device-reported error."""

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


def get_status(host: str, timeout: float = DASHBOARD_HTTP_TIMEOUT_S) -> dict:
    """GET /api/status and return the full decoded JSON object, straight
    from dashboard_status_handler(). Callers after only the heap figures
    should prefer get_heap_status() below; this is exposed for anything that
    needs more of the payload without a second GET."""
    req = urllib.request.Request(_url(host, "/api/status"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise DashboardHttpError(f"GET /api/status failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise DashboardHttpError(f"GET /api/status response was not valid JSON: {body_text!r}") from exc


#: 2026-08-31 incident: the board panicked at ~15:05 and ran five hours with
#: nobody noticing, including through this very function -- get_heap_status
#: called get_status(host) and discarded every key except the three heap
#: ones, so reset_reason (and uptime_s) passed through and were thrown away
#: even though they were sitting right there in the same response. This is
#: the read path every other diagnostic in this tree already calls; rather
#: than add a new tool nobody would think to call, these two keys are always
#: carried through from here on -- loud, not buried behind a separate GET.
_CARRY_THROUGH_KEYS = ("reset_reason", "uptime_s")

#: reset_reason values that mean "the board did not shut down cleanly" --
#: mirrors pid_validation.PANIC_RESET_REASONS's intent (that module compares
#: consecutive readings to detect a NEW restart; this one just flags
#: whatever the CURRENT boot's reason is, on every single read).
UNCLEAN_RESET_REASONS = frozenset({
    "panic/exception", "panic", "exception", "watchdog", "brownout",
})


def get_crash_report(host: str, timeout: float = DASHBOARD_HTTP_TIMEOUT_S) -> dict:
    """GET /api/crash_report and return the full decoded JSON object
    (diagnostics_http.c: crash_report_get_handler -- ``{"present": false}``
    when there is nothing on record, otherwise ``present/acknowledged/
    exc_task/exc_cause_str/...``).

    READ-ONLY: this module has no function that POSTs to
    ``/api/crash_report/ack`` or ``/api/crash_report/clear``, and none should
    be added here without a very deliberate reason -- a live crash report
    must stay exactly as unacknowledged as the board reports it until a
    person (or an explicit, separately-invoked tool) decides otherwise."""
    req = urllib.request.Request(_url(host, "/api/crash_report"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise DashboardHttpError(f"GET /api/crash_report failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise DashboardHttpError(
            f"GET /api/crash_report response was not valid JSON: {body_text!r}") from exc


def get_cfgfs_status(host: str, timeout: float = DASHBOARD_HTTP_TIMEOUT_S) -> dict:
    """GET /api/cfgfs and return the full decoded JSON object
    (diagnostics_http.c: cfgfs_status_get_handler()) -- observability for the
    `cfg` LittleFS partition (docs/FILESYSTEM_USER_DATA_PLAN.md): mounted/
    status/reason, capacity, the file list with sizes, how many entries are
    currently sitting in .tmp/ (see cfg_fs_status.c's own doc comment on why
    this is a live snapshot, not the historical at-mount reap count), and the
    zones-config dual-write picture (file_rev vs nvs_rev, and whether they
    have diverged)."""
    req = urllib.request.Request(_url(host, "/api/cfgfs"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise DashboardHttpError(f"GET /api/cfgfs failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise DashboardHttpError(f"GET /api/cfgfs response was not valid JSON: {body_text!r}") from exc


def get_diagnostics_timing(host: str, timeout: float = DASHBOARD_HTTP_TIMEOUT_S) -> dict:
    """GET /api/diagnostics/timing (diagnostics_http.c:
    diagnostics_timing_get_handler) -- HW_ABSTRACTION.md "Still open": display
    flush time, thermocouple read-cycle latency, and (2026-09-06) the ESP<->
    Pico safety-link reply latency, made reportable rather than requiring a
    bench session with a scope. Returns ``{"display_flush_us": {...},
    "thermo_read_us": {...}, "link_reply_us": {...}}``, each with
    count/last/min/max/mean in microseconds (link_reply_us also carries
    ``timeouts``, safety_link_stats_t's existing counter, not a new one);
    count == 0 means that path has not run yet on this boot (min/mean are 0
    until then, not a real measurement)."""
    req = urllib.request.Request(_url(host, "/api/diagnostics/timing"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise DashboardHttpError(f"GET /api/diagnostics/timing failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise DashboardHttpError(
            f"GET /api/diagnostics/timing response was not valid JSON: {body_text!r}") from exc


def get_heap_status(host: str, timeout: float = DASHBOARD_HTTP_TIMEOUT_S) -> dict:
    """GET /api/status and return the heap_internal/heap_spiram/heap_dma
    sub-objects (each {free, largest_free_block, min_free, total} in bytes),
    PLUS reset_reason/uptime_s carried through unchanged, PLUS a top-level
    ``unacknowledged_crash`` summary (``None`` when there is nothing to
    report, otherwise a dict with the crash-report fields) from one extra
    GET /api/crash_report -- see _CARRY_THROUGH_KEYS's comment for why this
    function, of all places, is where that lives now.

    Raises DashboardHttpError with a specific message (not a silent partial
    dict) if any of the three heap keys is missing from the /api/status
    response -- a firmware/tool version mismatch should be loud here, not
    read back as "0 bytes everywhere", which would look like the DRAM
    exhaustion this plan exists to measure. A missing/unreachable
    /api/crash_report endpoint (older firmware) is NOT treated as fatal for
    this call -- heap figures must still be reported -- but shows up as
    ``unacknowledged_crash_check_error`` rather than being silently dropped.
    Same non-fatal treatment for GET /api/diagnostics/timing (older firmware
    without it reports ``diagnostics_timing_check_error`` instead of the two
    timing blocks)."""
    status = get_status(host, timeout=timeout)
    result: dict = {}
    missing = []
    for key in _HEAP_KEYS:
        if key not in status:
            missing.append(key)
            continue
        result[key] = status[key]
    if missing:
        raise DashboardHttpError(
            f"GET /api/status response is missing {missing} -- "
            f"firmware/pc_tools version mismatch? (present keys: {sorted(status.keys())})"
        )
    for key in _CARRY_THROUGH_KEYS:
        result[key] = status.get(key)

    result["unacknowledged_crash"] = None
    try:
        crash = get_crash_report(host, timeout=timeout)
    except DashboardHttpError as exc:
        result["unacknowledged_crash_check_error"] = str(exc)
    else:
        if isinstance(crash, dict) and crash.get("present") and not crash.get("acknowledged", True):
            result["unacknowledged_crash"] = crash

    try:
        timing = get_diagnostics_timing(host, timeout=timeout)
    except DashboardHttpError as exc:
        result["diagnostics_timing_check_error"] = str(exc)
    else:
        if isinstance(timing, dict):
            result["display_flush_us"] = timing.get("display_flush_us")
            result["thermo_read_us"] = timing.get("thermo_read_us")
            # HW_ABSTRACTION.md "Still open": on-board ESP<->Pico safety-link
            # reply latency (safety_link.c's safety_exchange(), timed with
            # hal_time_now_us() -- see safety_link_stats_t::link_reply_us_
            # count's doc comment). Also carries a `timeouts` sub-field --
            # NOT a separate counter, it is safety_link_stats_t's existing
            # `timeouts` field surfaced under this block for convenience.
            result["link_reply_us"] = timing.get("link_reply_us")

    return result
