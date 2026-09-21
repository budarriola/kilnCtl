"""INFO tools (task 3).

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
import os
import subprocess
import sys
import threading
import time
from collections import deque
from typing import Any, Callable, Optional

from . import actions, config_presets, dashboard_http_client, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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
# INFO queries (task 3)
#
# Unlike the command tools these return real device data: INFO is a query
# channel, so the ACK only confirms delivery and the answer arrives in a
# separate DATA frame (see info.py).
# ---------------------------------------------------------------------------
@_srv._tool()
def get_pin_config() -> str:
    """Report which GPIOs the running firmware has wired up, and to what.

    Read live from the device, so it reflects what is actually flashed rather
    than a host-side table. Only real ESP32-S3 GPIOs appear here -- the relay
    drives, DRDY inputs and the display's D/C and ~RESET are expander pins,
    reported by io_read() instead.
    """
    try:
        entries = _srv._info.get_pin_config()
    except InfoQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "device reported no pin entries"
    return "\n".join(f"GPIO{e.gpio}: {e.label} [{e.abbrev}]" for e in entries)


@_srv._tool()
def get_stack_margin() -> str:
    """Report every instrumented task's stack high-water mark, live.

    This is the bench measurement `KilnFW/TODO.md` section 13 requires before
    any of the six internal-only task stacks it names may be resized. The
    figure is `uxTaskGetStackHighWaterMark()`: the SMALLEST free stack ever
    seen since that task started, not the current free amount -- so it is only
    as good as the worst path the task has actually taken since boot. Exercise
    a task's heavy path first (a big POST, an OTA, a config commit), then read
    this; a number taken from an idle board understates every stack.

    A task shown as "not running" was never created or has been deleted, and
    reports 0 rather than a stale earlier reading.
    """
    try:
        entries = _srv._info.get_stack_margin()
    except InfoQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "device reported no instrumented tasks"
    lines = []
    for e in entries:
        if not e.alive:
            lines.append(f"{e.name}: not running (configured {e.configured_stack_bytes} B)")
            continue
        pct = e.headroom_pct
        pct_txt = f"{pct:.1f}%" if pct is not None else "n/a"
        lines.append(
            f"{e.name}: {e.hwm_bytes} B free at worst of {e.configured_stack_bytes} B "
            f"({pct_txt} headroom) [{e.level.name}]"
        )
    return "\n".join(lines)


@_srv._tool()
def get_heap_status(host: Optional[str] = None) -> str:
    """Report internal-DRAM, PSRAM, and DMA-capable-internal-memory heap
    figures, live, over HTTP GET /api/status.

    DRAM_PSRAM_PLAN.md Phase 0 (4.1): the one MCP-side gap that plan's
    section identified -- dashboard_http.c has served heap_internal/
    heap_spiram/heap_dma in every /api/status response since before this
    tool existed, but nothing under tools/PcTools parsed those keys, so the
    data was reachable by curl or a browser and nowhere else. `min_free`
    (the low-water mark since boot) is the number every acceptance criterion
    in that plan is written against -- the instantaneous `free` figure is
    nearly useless, since the exhaustion event this plan exists to catch is
    transient and load-dependent. `heap_dma` is a STRICT SUBSET of
    `heap_internal` (both draw from MALLOC_CAP_INTERNAL), not new internal-
    DRAM information -- it answers "how much of that internal memory is
    DMA-capable", which matters once Phase 1 starts lowering
    SPIRAM_MALLOC_ALWAYSINTERNAL.

    Same host-resolution order as every ota_*/adaptive_tune_* tool
    (`_ota_resolve_host`): explicit `host` argument, else the STA IP if
    Wi-Fi reports one connected, else the board's own softAP address. This
    is a single GET per call, not a poll -- call it again for a fresh
    reading rather than looping here.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    resolved = _ota_resolve_host(host)
    try:
        heap = dashboard_http_client.get_heap_status(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: {exc} (host={resolved})"
    lines = [f"host={resolved}"]
    crash = heap.get("unacknowledged_crash")
    if crash:
        lines.append(
            "!!! UNACKNOWLEDGED CRASH REPORT !!! exc_task="
            f"{crash.get('exc_task')!r} exc_cause_str={crash.get('exc_cause_str')!r} "
            f"reset_reason={crash.get('found_on_boot_reset_reason')!r} -- "
            "this board panicked and nobody has reviewed it yet "
            "(GET /api/crash_report). Do not assume this run/board is healthy."
        )
    reset_reason = heap.get("reset_reason")
    if reset_reason in dashboard_http_client.UNCLEAN_RESET_REASONS:
        lines.append(f"!!! reset_reason={reset_reason!r} (unclean boot) uptime_s={heap.get('uptime_s')}")
    else:
        lines.append(f"reset_reason={reset_reason!r} uptime_s={heap.get('uptime_s')}")
    check_err = heap.get("unacknowledged_crash_check_error")
    if check_err:
        lines.append(f"(could not check /api/crash_report: {check_err})")
    for key in ("heap_internal", "heap_spiram", "heap_dma"):
        h = heap[key]
        lines.append(
            f"{key}: free={h['free']} B, largest_free_block={h['largest_free_block']} B, "
            f"min_free={h['min_free']} B (low-water since boot), total={h['total']} B"
        )
    timing_err = heap.get("diagnostics_timing_check_error")
    if timing_err:
        lines.append(f"(could not check /api/diagnostics/timing: {timing_err})")
    else:
        # HW_ABSTRACTION.md "Still open": display flush time and thermocouple
        # read-cycle latency, both in microseconds -- see diagnostics_http.c's
        # diagnostics_timing_get_handler(). count==0 means that path has not
        # run yet on this boot (min/mean report as 0 until then, not a real
        # zero-length measurement).
        for label, key in (("display_flush_us", "display_flush_us"), ("thermo_read_us", "thermo_read_us"),
                           ("link_reply_us", "link_reply_us")):
            t = heap.get(key)
            if not t:
                continue
            if t.get("count", 0) == 0:
                lines.append(f"{label}: no samples yet")
            else:
                line = (
                    f"{label}: count={t['count']} last={t['last']} min={t['min']} "
                    f"max={t['max']} mean={t['mean']} (us)"
                )
                # link_reply_us only: `timeouts` is safety_link_stats_t's
                # existing counter, surfaced here rather than as a separate
                # metric -- see dashboard_http_client.py's
                # get_diagnostics_timing() doc comment. REDEFINED 2026-09-10
                # (docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md
                # and its follow-up review): no longer "requests with no
                # matching reply" (GET_STATUS never gets a matching reply by
                # design, so that definition either always read ~0% or
                # ~100% on a healthy link, or, in a first attempted fix,
                # could not exceed what the link's own age-based liveness
                # check already showed); now a count of ~500 ms poll
                # iterations with zero new STATUS frames applied anywhere --
                # near zero when healthy, rising under real partial loss.
                if key == "link_reply_us" and "timeouts" in t:
                    line += f" timeouts={t['timeouts']}"
                lines.append(line)
    return "\n".join(lines)


def _describe_crash_report(rec: dict) -> str:
    """One-line-per-field summary of a GET /api/crash_report record with
    ``present: true`` -- the same fields get_heap_status()'s
    UNACKNOWLEDGED CRASH REPORT banner surfaces, plus exc_pc, since this is
    the point where an operator/agent decides whether the record is safe to
    dismiss."""
    return (
        f"reset_reason={rec.get('found_on_boot_reset_reason')!r} "
        f"exc_task={rec.get('exc_task')!r} exc_cause_str={rec.get('exc_cause_str')!r} "
        f"exc_pc={rec.get('exc_pc')!r} exc_addr={rec.get('exc_addr')!r} "
        f"already_acknowledged={rec.get('acknowledged')}"
    )


@_srv._tool()
def crash_report_ack(confirm: bool = False, host: Optional[str] = None) -> str:
    """Acknowledge the board's last-crash record (POST
    /api/crash_report/ack, diagnostics_http.c's crash_report_ack_post_
    handler(), ROUTE_TIER_ADMIN) -- the same action the diagnostics page's
    "Acknowledge" button performs, and the same one get_heap_status()'s
    "UNACKNOWLEDGED CRASH REPORT" banner and capability_preflight() are
    checking for. This does NOT erase the coredump image (that is
    ``/api/crash_report/clear``, which this tool never calls) -- only marks
    the record reviewed.

    Always fetches the CURRENT report first (GET /api/crash_report). If no
    crash is on record (``present: false``) or the record is already
    acknowledged, this returns that and does nothing else -- no POST is ever
    sent for a record that is not both present and unacknowledged.

    Otherwise the report's summary (reset reason, task, exception cause/PC,
    whether the backtrace frame is trustworthy) is put in the result FIRST,
    before anything is acknowledged, so a caller reading the result sees
    what it is about to dismiss rather than a bare "ok".

    REFUSES UNLESS ``confirm=True`` -- without it, this is a dry run: it
    reports the pending record (or "nothing pending") and says what it
    WOULD acknowledge, but sends no POST. This mirrors safety_set_rate_
    guard()'s confirm gate, for the same reason: acknowledging a crash
    nobody has actually read defeats the entire point of the banner that
    led here.

    With ``confirm=True``, POSTs the ack (crash_report_ack_http_client.py,
    over the same web-auth seam every other ADMIN-tier write tool in this
    package uses -- KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD from the
    environment, http_auth.urlopen()'s one-retry-after-401 login), then
    re-fetches GET /api/crash_report and FAILS LOUDLY (does not report
    success) if the record still reads ``acknowledged: false`` afterward --
    an ``{"ok":true}`` POST reply is not trusted alone, same rule
    safety_cfg_http_client.apply_safety_fields()'s verify=True path and
    safety_set_tc_type()/safety_set_rate_guard()'s docstrings already state
    for this codebase's write tools generally (see CLAUDE.md's boot_guard
    write-lies section for why an unverified success report is exactly the
    failure class this project has been bitten by before).

    A 409 ("no crash record to acknowledge") or 500 ("failed to persist
    acknowledgement") from the board is reported as a failure naming which
    one, distinguished by crash_report_ack_http_client's
    CrashReportAckHttpError.status -- never collapsed into a single generic
    error string.

    Host is auto-resolved the same way get_heap_status()/the OTA/control
    tools do; pass `host` explicitly for kilnctl.local or a board reachable
    only from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_heap_status()
    from . import crash_report_ack_http_client

    resolved = _ota_resolve_host(host)
    try:
        before = dashboard_http_client.get_crash_report(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: could not read GET /api/crash_report (host={resolved}): {exc}"

    if not before.get("present"):
        return f"nothing pending -- GET /api/crash_report reports present=false (host={resolved})"

    summary = _describe_crash_report(before)
    if before.get("acknowledged"):
        return f"already acknowledged, nothing to do -- {summary} (host={resolved})"

    if not confirm:
        return (
            f"DRY RUN (pass confirm=True to actually acknowledge) -- pending crash: {summary} "
            f"(host={resolved})"
        )

    try:
        crash_report_ack_http_client.post_crash_report_ack(resolved)
    except crash_report_ack_http_client.CrashReportAckHttpError as exc:
        if exc.status == 409:
            return (f"failed: board reports no crash record to acknowledge (409) even though "
                     f"the pre-fetch above saw one -- {summary} (host={resolved}): {exc}")
        if exc.status == 500:
            return (f"failed: board could not persist the acknowledgement (500) -- record is "
                     f"still unacknowledged -- {summary} (host={resolved}): {exc}")
        return f"error acknowledging crash report over HTTP (host={resolved}): {exc}"

    try:
        after = dashboard_http_client.get_crash_report(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return (f"error: POST /api/crash_report/ack returned ok, but the confirming re-fetch "
                f"failed (host={resolved}): {exc} -- acknowledgement state UNKNOWN, re-check "
                f"before trusting this")

    if not after.get("present") or after.get("acknowledged"):
        return f"ok - acknowledged and confirmed by read-back: {summary} (host={resolved})"
    return (f"FAILED: POST /api/crash_report/ack returned ok, but the re-fetched record still "
            f"reads acknowledged=false -- {summary} (host={resolved}). Do not trust this as "
            f"acknowledged.")


@_srv._tool()
def get_readiness(host: Optional[str] = None) -> str:
    """READ-ONLY: fetch and render the commissioning checklist from GET
    /api/readiness (readiness_http.c's api_readiness_get_handler(), the
    same data the board's own readiness page renders). Pure GET, no side
    effects -- safe to call at any time, including during a live firing.

    Each item is ``{key, label, status, detail, fix_url}``; `status` is one
    of "ok", "not_done", "cannot_yet", "deliberately_off" (readiness_
    status_t's four required distinctions -- see readiness_http.c's own
    comment: "not_done" means an operator step is outstanding, "cannot_yet"
    means a prerequisite item is blocking this one, "deliberately_off"
    means an operator chose to skip it, not that it is broken). Renders one
    line per item as ``STATUS key: detail`` (fix_url appended when present),
    followed by a summary count of ok / not_done / other (cannot_yet +
    deliberately_off + anything unrecognised).

    Host is auto-resolved the same way get_heap_status()/the OTA/control
    tools do; pass `host` explicitly for kilnctl.local or a board reachable
    only from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_heap_status()
    from . import readiness_http_client

    resolved = _ota_resolve_host(host)
    try:
        data = readiness_http_client.get_readiness(resolved)
    except readiness_http_client.ReadinessHttpError as exc:
        return f"error reading readiness over HTTP (host={resolved}): {exc}"

    items = data.get("items", [])
    lines = [f"host={resolved}"]
    ok_count = 0
    not_done_count = 0
    other_count = 0
    for item in items:
        key = item.get("key", "?")
        status = item.get("status", "?")
        detail = item.get("detail", "")
        fix_url = item.get("fix_url", "")
        line = f"{status} {key}: {detail}"
        if fix_url:
            line += f" (fix: {fix_url})"
        lines.append(line)
        if status == "ok":
            ok_count += 1
        elif status == "not_done":
            not_done_count += 1
        else:
            other_count += 1
    lines.append(f"summary: {ok_count} ok, {not_done_count} not_done, {other_count} other "
                 f"({len(items)} total)")
    return "\n".join(lines)


@_srv._tool()
def fetch_event_log(kind: str, host: Optional[str] = None) -> str:
    """Fetch and decode the board's on-flash binary event log, over HTTP
    GET /api/logs/{firing,autotune}.

    2026-09-17 vacuity sweep finding: kilnctrl.event_log_decoder.py (the
    documented PC-side counterpart to event_log.c, see FLASH_BUDGET.md sec
    5.2) had a full test suite but no production caller anywhere in this
    tree -- nothing fetched a real board's event log and ran it through the
    decoder. This is that caller.

    ``kind`` is "firing" or "autotune" (log_http.c has no third kind). Same
    host-resolution order as get_heap_status/every ota_*/adaptive_tune_*
    tool: explicit `host` argument, else the STA IP if Wi-Fi reports one
    connected, else the board's own softAP address.

    Returns one human-readable line per decoded record (oldest first, as
    the board stores them), or an ``error:`` string on a transport failure
    or an unrecognized/old-format log (event_log_decoder refuses rather
    than misreading a pre-2026-09-02 text-format log -- see that module's
    migration note). An empty log (nothing logged yet, or freshly erased)
    reports that explicitly rather than an empty string, so it isn't
    mistaken for a fetch failure.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py
    from . import event_log_decoder

    kind = kind.strip().lower()
    resolved = _ota_resolve_host(host)
    try:
        raw = dashboard_http_client.get_event_log_bytes(resolved, kind)
    except ValueError as exc:
        return f"error: {exc}"
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: {exc} (host={resolved})"
    try:
        records = event_log_decoder.decode_stream(raw)
    except event_log_decoder.EventLogFormatError as exc:
        return f"error: could not decode {kind} event log from {resolved}: {exc}"
    if not records:
        return f"host={resolved} kind={kind}: log is empty (0 records)"
    lines = [f"host={resolved} kind={kind}: {len(records)} record(s)"]
    lines.extend(event_log_decoder.format_record(r) for r in records)
    return "\n".join(lines)


@_srv._tool()
def get_cfgfs_status(host: Optional[str] = None) -> str:
    """Report the `cfg` LittleFS partition's live state, over HTTP GET
    /api/cfgfs (diagnostics_http.c: cfgfs_status_get_handler()).

    docs/FILESYSTEM_USER_DATA_PLAN.md's user-data-on-a-filesystem migration
    left the `cfg` partition otherwise invisible -- mounted or not, how full,
    what files exist, and whether the zones-config dual-write's file and NVS
    copies agree were only findable by grepping the boot log. This is the
    fix: mounted/status/reason, capacity (total/used/free bytes, "unknown" if
    esp_littlefs_info() itself failed -- never reported as a fake zero),
    file_count/files (name + size_bytes), tmp_entries_now (files currently
    sitting in `.tmp/` -- nonzero shortly after boot on an otherwise-idle
    board suggests an interrupted write; see cfgfs's own field-level doc for
    why this is a live snapshot, not the historical count reaped at mount),
    and dual_write.zones (file_backed/file_rev/nvs_rev/diverged -- diverged
    means a prior file write failed and only NVS advanced) plus nvs_only, the
    list of items docs/FILESYSTEM_USER_DATA_PLAN.md section 5 has not yet
    migrated off NVS.

    Same host-resolution order as get_heap_status()."""
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    resolved = _ota_resolve_host(host)
    try:
        cfgfs = dashboard_http_client.get_cfgfs_status(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: {exc} (host={resolved})"

    lines = [f"host={resolved}", f"mounted={cfgfs.get('mounted')} status={cfgfs.get('status')!r}"]
    if not cfgfs.get("mounted"):
        lines.append(f"reason: {cfgfs.get('reason')}")

    cap = cfgfs.get("capacity") or {}
    if cap.get("known"):
        lines.append(
            f"capacity: total={cap.get('total_bytes')} B used={cap.get('used_bytes')} B "
            f"free={cap.get('free_bytes')} B"
        )
    else:
        lines.append("capacity: unknown")

    files = cfgfs.get("files") or []
    lines.append(f"file_count={cfgfs.get('file_count')}")
    for f in files:
        size = f.get("size_bytes")
        lines.append(f"  {f.get('name')}: {size if size is not None else 'unknown'} B")

    tmp_now = cfgfs.get("tmp_entries_now")
    if tmp_now:
        lines.append(f"!!! tmp_entries_now={tmp_now} -- a write may be interrupted or repeatedly failing")
    else:
        lines.append(f"tmp_entries_now={tmp_now}")

    dual = cfgfs.get("dual_write") or {}
    zones = dual.get("zones") or {}
    if zones.get("file_backed"):
        diverged_note = " !!! DIVERGED -- a prior file write failed, only NVS advanced" if zones.get("diverged") else ""
        lines.append(f"dual_write.zones: file_rev={zones.get('file_rev')} nvs_rev={zones.get('nvs_rev')}{diverged_note}")
    else:
        lines.append("dual_write.zones: not file-backed yet (NVS only)")
    nvs_only = dual.get("nvs_only") or []
    if nvs_only:
        lines.append(f"still NVS-only: {', '.join(nvs_only)}")

    return "\n".join(lines)


@_srv._tool()
def get_fw_version() -> str:
    """Report the running firmware's git commit, dirty flag, build time, and
    whether its UART protocol version matches this copy of pc_tools.

    Call this before any device tool: they all refuse to send until a
    compatible version has been confirmed this way (or via a boot push). This
    gate matters here -- a v1 firmware is the unit-test fixture, where task 1
    is a DAC rather than three thermocouples.
    """
    try:
        version = _srv._info.get_fw_version()
    except InfoQueryError as exc:
        return f"error: {exc}"
    compat = (
        "yes"
        if version.compatible
        else f"NO - device speaks v{version.protocol_version}, pc_tools speaks "
        f"v{devices.UART_PROTOCOL_VERSION}; device commands will be refused"
    )
    lines = [
        # Labelled uart_protocol_version, not protocol_version: this is the
        # PC<->ESP bench-link number (uart_task_ids.h's UART_PROTOCOL_VERSION,
        # which the compat gate just above compares against), NOT CommonFW's
        # KILNLINK_PROTOCOL_VERSION carried by /api/status's
        # self_protocol_version for the ESP<->Pico link. The two are
        # deliberately independent since uart_task_ids.h stopped aliasing
        # kilnlink_version.h, so they legitimately read different (11 vs 12 as
        # of e584067f) and the bare old name invited reading that as a stale
        # source.
        f"uart_protocol_version: {version.protocol_version}",
        f"compatible: {compat}",
        f"commit: {version.commit}",
        f"tree: {'dirty' if version.dirty else 'clean'}",
        f"built: {version.built}",
    ]
    # Board-vs-HEAD gap and last-refused-flash checks (added 2026-09-04: a
    # refused flash -- the fix never reached the board -- went unnoticed for
    # five hours because nothing compared what the board reports against
    # what the tree actually has, and the refusal itself left no durable
    # trace. Both checks are best-effort and must never break this tool.
    from . import elf_archive, flash_provenance  # local import: avoids a circular import with mcp_server_flash.py
    try:
        lines.append(flash_provenance.describe_head_gap(version.commit))
    except Exception as exc:  # noqa: BLE001 - this line is a bonus, not the tool's job
        lines.append(f"board/HEAD comparison: error computing it ({exc})")
    try:
        warning = _read_last_flash_warning()
        if warning:
            lines.append(warning)
    except Exception as exc:  # noqa: BLE001 - same, bonus info
        lines.append(f"(could not check last flash outcome: {exc})")
    return "\n".join(lines)


def _read_last_flash_warning() -> Optional[str]:
    """Reads flash_provenance.json and formats the last-flash warning line
    (if any) -- factored out of get_fw_version() so it can be unit tested
    without a live board/link.

    M1 (2026-09-15 review): flash_provenance.json moved out of build/ -- see
    elf_archive.kiln_provenance_path()'s docstring.

    L3 (2026-09-15 fixes review): a file left behind at the OLD path (from
    before that move, or from a stale server still writing there -- see
    CLAUDE.md "Stale-server self-announcing") used to go completely unread
    once the new path was checked exclusively, silently dropping the
    last-flash warning. Migrate it into place if present; if that can't
    happen (e.g. guarded during a test), fall back to reading the legacy
    path directly rather than reporting nothing."""
    from . import elf_archive, flash_provenance  # local import: avoids a circular import with mcp_server_flash.py
    prov_path = elf_archive.kiln_provenance_path()
    if not os.path.isfile(prov_path):
        try:
            elf_archive.migrate_legacy_provenance()
        except Exception:  # noqa: BLE001 - migration is best-effort here
            pass
    if os.path.isfile(prov_path):
        prov = flash_provenance.read_provenance_json(prov_path)
    else:
        legacy_path = elf_archive.legacy_kiln_provenance_path()
        prov = flash_provenance.read_provenance_json(legacy_path) if os.path.isfile(legacy_path) else None
    return flash_provenance.format_last_flash_warning(prov)


