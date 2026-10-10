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

from . import mcp_server_core as _core


# ---------------------------------------------------------------------------
# INFO queries (task 3)
#
# Unlike the command tools these return real device data: INFO is a query
# channel, so the ACK only confirms delivery and the answer arrives in a
# separate DATA frame (see info.py).
# ---------------------------------------------------------------------------
@_core._tool()
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


@_core._tool()
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


@_core._tool()
def check_task_liveness() -> str:
    """READ-ONLY: cross-check the board's live GET_STACK_MARGIN reading
    against the required-task list `tools/check_stack_margin_registration.ps1`
    enforces at the source level.

    That .ps1 check only proves every required task HAS a
    `stack_margin_register()` call site in the firmware source -- it says
    nothing about whether `xTaskCreate*()` actually succeeded for each one
    on a given boot. Every KilnFW task-creation failure is log-only
    (`ESP_LOGE`, non-fatal, no counter, nothing HTTP-visible), so a board
    that silently failed to start a required task at boot looks perfectly
    healthy everywhere else. `stack_margin_read()` is the one place that
    failure stays visible: the task's registry slot is present (registered
    by name) but its handle is NULL, so `alive` reads false.

    Reports:
      - expected-and-alive: registered and running, as expected.
      - expected-but-DEAD: registered (a call site exists and fired) but
        `alive=False` -- task creation failed THIS boot. FATAL for a plain
        (untagged/`always`) task; informational for one tagged `config`,
        `on-demand`, or `boot-once` in `$requiredNames` (a by-design gap --
        conditional on build config/hardware, transient, or a one-shot boot
        task that has since self-deleted).
      - expected-but-ABSENT: never appeared in the reply at all -- older
        firmware, or a code regression dropped its registration. FATAL for
        `always`/`boot-once`; informational for `config`/`on-demand`.
      - extra: alive tasks not in the expected list -- informational only.

    The expected-task list, and each name's liveness tag, is parsed live
    from `tools/check_stack_margin_registration.ps1`'s own `$requiredNames`
    array (a trailing `# liveness: <tag>` comment on the entry's line), not
    copied into a second, driftable list -- see `task_liveness.py`'s module
    docstring.
    """
    try:
        entries = _srv._info.get_stack_margin()
    except InfoQueryError as exc:
        return f"error: {exc}"
    from . import task_liveness

    repo_root = os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
    )
    script_path = task_liveness.default_check_script_path(repo_root)
    try:
        specs = task_liveness.load_required_task_specs(script_path)
    except (task_liveness.TaskLivenessParseError, OSError) as exc:
        return f"error: could not load required-task list from {script_path}: {exc}"
    expected = tuple(spec.name for spec in specs)
    tags = {spec.name: spec.tag for spec in specs}
    report = task_liveness.check_task_liveness(entries, expected, tags=tags)
    return report.describe()


@_core._tool()
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
        uptime_bit = (
            f"crash_uptime_s={crash.get('crash_uptime_s')}"
            if crash.get("crash_uptime_known")
            else "crash_uptime_s=unknown"
        )
        stale_note = dashboard_http_client.crash_report_stale_note(crash)
        if stale_note:
            lines.append(f"!!! {stale_note} !!!")
        lines.append(
            "!!! UNACKNOWLEDGED CRASH REPORT !!! exc_task="
            f"{crash.get('exc_task')!r} exc_cause_str={crash.get('exc_cause_str')!r} "
            f"reset_reason={crash.get('found_on_boot_reset_reason')!r} "
            f"fw_build={crash.get('fw_build')!r} {uptime_bit} dump_id={crash.get('dump_id')} -- "
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
    low = heap.get("heap_internal_largest_low")
    if isinstance(low, dict):
        lines.append(
            f"heap_internal largest_free_block low-water: {low.get('bytes')} B first seen at "
            f"uptime_s={low.get('at_uptime_s')} (SK-04 alarm 8704 B)"
        )
    else:
        lines.append("heap_internal largest_free_block low-water: not reported (older firmware or not sampled yet)")
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
    uptime_bit = (
        f"crash_uptime_s={rec.get('crash_uptime_s')}"
        if rec.get("crash_uptime_known")
        else "crash_uptime_s=unknown"
    )
    stale_note = dashboard_http_client.crash_report_stale_note(rec)
    return (
        (f"[{stale_note}] " if stale_note else "")
        + f"reset_reason={rec.get('found_on_boot_reset_reason')!r} "
        f"exc_task={rec.get('exc_task')!r} exc_cause_str={rec.get('exc_cause_str')!r} "
        f"exc_pc={rec.get('exc_pc')!r} exc_addr={rec.get('exc_addr')!r} "
        f"fw_build={rec.get('fw_build')!r} {uptime_bit} dump_id={rec.get('dump_id')} "
        f"already_acknowledged={rec.get('acknowledged')}"
    )


@_core._tool()
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

    if confirm is not True:
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


def _readiness_item(data: dict, key: str) -> Optional[dict]:
    for item in data.get("items", []):
        if item.get("key") == key:
            return item
    return None


@_core._tool()
def estop_verify(confirm: bool = False, host: Optional[str] = None) -> str:
    """Record that a HUMAN has physically verified the E-stop interlock
    (POST /api/estop/verify, diagnostics_http.c's estop_verify_post_
    handler(), ROUTE_TIER_ADMIN) -- the deliberate operator confirmation
    firmware/SaftyFW/README.md's bench verification procedure ends with:
    "I have verified the E-stop interlock" (both poles, on this fixture
    pole 2; pole 1, the external line contactor's coil circuit, is wiring
    firmware cannot see, which is the whole reason this flag is never
    inferred from a GPIO read or a config value -- see estop_verification.h
    for what invalidates it again).

    THIS TOOL MUST NEVER BE CALLED FROM AN AUTOMATED SUITE. A passing test
    or a script noticing readiness is "not_done" is not a substitute for a
    person actually operating the E-stop and observing it cut power --
    calling this without that having happened records a false attestation.
    tools/PcTools/src/kilnctrl/bench_test/judgments.py's judge_estop_verify()
    (SP-05) explicitly documents this same rule for the read-only smoke
    case it covers and never calls this route itself; this tool exists for
    a human (or an agent explicitly told a human just performed the
    physical check) to invoke deliberately, once, after that check.

    Always fetches GET /api/readiness FIRST and reports the ``estop_
    verified`` item's current status before doing anything. If the
    ``safety_trip`` item is not ``ok`` -- INCLUDING when it is simply
    absent from the response, since readiness_http.c's item buffer can
    fill and silently drop entries -- this refuses unconditionally,
    regardless of `confirm`: an E-stop verification must only ever be
    recorded with a confirmed-ok safety_trip, never on the assumption that
    a missing item means no trip. If readiness itself cannot be read, this
    errors before touching the write route at all.

    REFUSES UNLESS ``confirm=True`` (exactly ``True``) -- without it, this
    is a dry run: it reports the current readiness state and says what it
    WOULD record, but sends no POST. Same confirm-gate convention as
    crash_report_ack()/crash_report_clear().

    With ``confirm=True``, POSTs the verification
    (estop_verify_http_client.py, over the same http_auth.urlopen() ADMIN-
    session seam every other ADMIN-tier write tool in this package uses --
    KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD from the environment), then
    re-fetches GET /api/readiness and FAILS LOUDLY (does not report
    success) if the ``estop_verified`` item still does not read ``ok``
    afterward -- an ``{"ok":true}`` POST reply is not trusted alone, same
    rule crash_report_ack()'s own docstring gives (see CLAUDE.md's
    boot_guard write-lies section for why an unverified success report is
    exactly the failure class this project has been bitten by before).

    A 500 ("estop_verification_confirm() failed") from the board is
    reported as a failure naming it, via estop_verify_http_client's
    EstopVerifyHttpError.status.

    Never prints, logs, or echoes a credential.

    Host is auto-resolved the same way get_readiness()/get_heap_status()
    do; pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()
    from . import readiness_http_client
    from . import estop_verify_http_client

    resolved = _ota_resolve_host(host)
    try:
        before = readiness_http_client.get_readiness(resolved)
    except readiness_http_client.ReadinessHttpError as exc:
        return f"error: could not read GET /api/readiness (host={resolved}): {exc}"

    estop_item = _readiness_item(before, "estop_verified")
    trip_item = _readiness_item(before, "safety_trip")
    estop_summary = (f"estop_verified status={estop_item.get('status')!r} "
                      f"detail={estop_item.get('detail')!r}" if estop_item is not None
                      else "estop_verified: item not present in readiness response")
    trip_summary = (f"safety_trip status={trip_item.get('status')!r} detail={trip_item.get('detail')!r}"
                     if trip_item is not None else "safety_trip: item not present in readiness response")

    if trip_item is None or trip_item.get("status") != "ok":
        return (f"refused: safety_trip is not confirmed ok ({trip_summary}) -- readiness may "
                f"have dropped the item (its buffer can fill and drop entries, per readiness_"
                f"http.c's dropped-item notice) or a trip may be latched; an E-stop verification "
                f"must never be recorded without a confirmed-ok safety_trip (host={resolved})")

    if confirm is not True:
        return (
            f"DRY RUN (pass confirm=True, exactly, to actually record) -- {estop_summary}; "
            f"{trip_summary} (host={resolved})"
        )

    try:
        estop_verify_http_client.post_estop_verify(resolved)
    except estop_verify_http_client.EstopVerifyHttpError as exc:
        return f"failed: POST /api/estop/verify error (host={resolved}): {exc}"

    try:
        after = readiness_http_client.get_readiness(resolved)
    except readiness_http_client.ReadinessHttpError as exc:
        return (f"error: POST /api/estop/verify returned ok, but the confirming re-fetch of "
                f"GET /api/readiness failed (host={resolved}): {exc} -- verification state "
                f"UNKNOWN, re-check before trusting this")

    after_item = _readiness_item(after, "estop_verified")
    after_summary = (f"estop_verified status={after_item.get('status')!r} "
                      f"detail={after_item.get('detail')!r}" if after_item is not None
                      else "estop_verified: item not present in re-fetched readiness response")
    if after_item is not None and after_item.get("status") == "ok":
        return (f"ok - recorded and confirmed by read-back: {after_summary} "
                f"(before: {estop_summary}) (host={resolved})")
    return (f"FAILED: POST /api/estop/verify returned ok, but the re-fetched readiness item "
            f"still does not read ok -- {after_summary} (before: {estop_summary}) "
            f"(host={resolved}). Do not trust this as recorded.")


_CLEAR_DEADLINE_S = 30.0  # bound on busy-retry plus completion polling in crash_report_clear()
_CLEAR_POLL_S = 1.0


def _clear_sleep(seconds: float) -> None:  # patched out in unit tests
    time.sleep(seconds)


@_core._tool()
def crash_report_clear(confirm: bool = False, allow_unacknowledged: bool = False,
                        host: Optional[str] = None) -> str:
    """Acknowledge AND erase the board's last-crash record AND/OR a stale
    coredump image (POST /api/crash_report/clear, diagnostics_http.c's
    crash_report_clear_post_handler(), ROUTE_TIER_ADMIN) -- the same action
    crash_report_clear() performs on the board: crash_report_acknowledge()
    followed by hal_sysinfo_coredump_erase(), freeing the `coredump`
    partition slot so an OLD coredump is never re-captured/re-reported
    after a reflash. This is a strictly more destructive action than
    crash_report_ack(), which never touches the coredump image -- use
    crash_report_ack() when only dismissing the banner is wanted.

    2026-09-22 fix: this tool used to gate entirely on GET /api/crash_
    report's ``present`` -- the NVS record -- and refused to run at all on
    a board with a stale coredump IMAGE but no crash record (e.g. after the
    record was already acknowledged/cleared on a previous pass, or a
    quarantined/corrupted record that ``load()`` in crash_report.c silently
    treats as "no record"). "Free the coredump partition before a reflash"
    is this tool's whole motivation, so it now ALSO fetches
    GET /api/coredump/info (coredump_fetch.get_coredump_info(), the same
    read read_esp_coredump() uses) and proceeds when EITHER the NVS record
    OR the coredump image is present -- never requiring both.

    Always fetches BOTH the current record (GET /api/crash_report) and the
    current image info (GET /api/coredump/info) first. If NEITHER is
    present, this returns that and does nothing else -- no POST is ever
    sent when there is nothing to clear.

    Otherwise a summary of both -- the record's details (or "no crash
    record present") and the image's presence/data_len/partition_size --
    is put in the result FIRST, before anything is cleared, so a caller
    sees what it is about to erase.

    REFUSES to clear an UNACKNOWLEDGED record unless
    ``allow_unacknowledged=True`` is ALSO passed, regardless of
    ``confirm`` -- a bench agent must not erase a crash nobody has
    actually reviewed. This gate only applies when a record is present;
    an image-only clear (no NVS record at all) is not gated by it, since
    there is no record to have reviewed.

    REFUSES UNLESS ``confirm=True`` -- without it, this is a dry run: it
    reports what is pending (record and/or image) and says what it WOULD
    clear, but sends no POST. Same rule crash_report_ack()/safety_
    set_rate_guard() use.

    With ``confirm=True`` (and, if needed, ``allow_unacknowledged=True``),
    POSTs the clear (crash_report_clear_http_client.py, over the same
    web-auth seam every other ADMIN-tier write tool in this package uses),
    then re-fetches BOTH GET /api/crash_report and GET /api/coredump/info
    and FAILS LOUDLY (does not report success) if EITHER still reads
    present afterward -- an ``{"ok":true}`` POST reply is not trusted
    alone, same rule crash_report_ack()'s own docstring gives (see
    CLAUDE.md's boot_guard write-lies section for why an unverified
    success report is exactly the failure class this project has been
    bitten by before). Note ``hal_sysinfo_coredump_erase()`` (esp_core_
    dump_image_erase()) can itself answer non-OK on an already-empty
    partition -- if the board's own erase 500s on a partition this tool's
    own pre-fetch already saw as not-present, that is reported as the
    ordinary 500 failure below, not silently swallowed.

    A 500 ("coredump erase failed" -- the ONLY failure the board's own
    crash_report_clear() propagates; a failed NVS erase of the crash
    record is merely logged there and still answers 200, which is the
    second reason the read-back below is not optional) is reported as a
    failure, distinguished by crash_report_clear_http_client's
    CrashReportClearHttpError.status -- never collapsed into a single
    generic error string. Unlike /ack, this route has no separate 409
    "nothing to do" status -- crash_report_clear() always attempts the
    erase, so a POST is only ever sent here when the pre-fetch already
    confirmed the record and/or the image is present.

    2026-10-02 (ROADMAP A3): the board now runs the erase on an
    http_async_job task (httpd_worker is no longer stalled ~3.4 s by it).
    A 503 (another async job running) is retried every second; a 202 or a
    POST socket timeout switches to polling the read-back
    (GET /api/crash_report's ``clear_in_progress`` plus /api/coredump/info)
    until both read gone or a 30 s deadline passes. A record or image still
    present, or clear_in_progress still true, at the end is a FAILURE.

    Host is auto-resolved the same way get_heap_status()/crash_report_ack()
    do; pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as crash_report_ack()
    from . import coredump_fetch, crash_report_clear_http_client

    resolved = _ota_resolve_host(host)
    try:
        before = dashboard_http_client.get_crash_report(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: could not read GET /api/crash_report (host={resolved}): {exc}"

    try:
        image_before = coredump_fetch.get_coredump_info(resolved)
    except coredump_fetch.CoredumpFetchError as exc:
        return f"error: could not read GET /api/coredump/info (host={resolved}): {exc}"

    def _image_summary(info) -> str:
        return (f"coredump image: present={info.present} data_len=0x{info.data_len:08x} "
                f"partition_size=0x{info.partition_size:08x}")

    record_present = bool(before.get("present"))
    image_present = image_before.present
    image_summary = _image_summary(image_before)

    if not record_present and not image_present:
        return (f"nothing pending -- no crash record and no coredump image present "
                f"({image_summary}) (host={resolved})")

    summary = _describe_crash_report(before) if record_present else "no crash record present"

    if record_present and not before.get("acknowledged") and not allow_unacknowledged:
        return (
            f"REFUSED: record is present but NOT acknowledged -- pass allow_unacknowledged=True "
            f"to clear an unreviewed crash anyway -- {summary}; {image_summary} (host={resolved})"
        )

    if confirm is not True:
        return (
            f"DRY RUN (pass confirm=True to actually clear) -- {summary}; {image_summary} "
            f"(host={resolved})"
        )

    # ROADMAP A3: the board runs the erase on the http_async_job task and
    # answers 503 while another job is running. Retry a busy answer and poll
    # the read-back after a 202/timeout, all inside one bounded deadline.
    deadline = time.monotonic() + _CLEAR_DEADLINE_S
    poll_readback = False
    post_timed_out = False
    while True:
        try:
            reply = crash_report_clear_http_client.post_crash_report_clear(resolved)
            poll_readback = bool(isinstance(reply, dict) and reply.get("accepted"))
            break
        except crash_report_clear_http_client.CrashReportClearBusy as exc:
            if time.monotonic() >= deadline:
                return (f"error: board stayed busy (503, another async job running) for "
                        f"{_CLEAR_DEADLINE_S:.0f}s -- nothing was cleared (host={resolved}): {exc}")
            _clear_sleep(_CLEAR_POLL_S)
        except crash_report_clear_http_client.CrashReportClearTimeout:
            poll_readback = True  # the erase may still be running -- poll, do not assume
            post_timed_out = True
            break
        except crash_report_clear_http_client.CrashReportClearHttpError as exc:
            if exc.status == 500:
                return (f"failed: board could not erase the coredump image (500) -- {summary}; "
                         f"{image_summary} (host={resolved}): {exc}")
            return f"error clearing crash report over HTTP (host={resolved}): {exc}"

    while True:
        try:
            after = dashboard_http_client.get_crash_report(resolved)
            image_after = coredump_fetch.get_coredump_info(resolved)
        except (dashboard_http_client.DashboardHttpError, coredump_fetch.CoredumpFetchError) as exc:
            return (f"error: POST /api/crash_report/clear returned ok, but the confirming re-fetch "
                    f"failed (host={resolved}): {exc} -- clear state UNKNOWN, re-check before "
                    f"trusting this")
        record_gone = not after.get("present")
        image_gone = not image_after.present
        in_progress = bool(after.get("clear_in_progress"))
        if (poll_readback or in_progress) and not (record_gone and image_gone and not in_progress) \
                and time.monotonic() < deadline:
            _clear_sleep(_CLEAR_POLL_S)
            continue
        break

    if record_gone and image_gone and not in_progress:
        return (f"ok - cleared and confirmed by read-back: {summary}; {_image_summary(image_after)} "
                 f"(host={resolved})")
    still_present = []
    if not record_gone:
        still_present.append("crash record still present=true")
    if not image_gone:
        still_present.append("coredump image still present=true")
    if in_progress:
        still_present.append(f"clear_in_progress still true after {_CLEAR_DEADLINE_S:.0f}s")
    post_said = ("timed out with no reply (outcome unknown)" if post_timed_out
                 else "returned ok")
    return (f"FAILED: POST /api/crash_report/clear {post_said}, but the re-fetch shows "
            f"{'; '.join(still_present)} -- {summary}; {_image_summary(image_after)} "
            f"(host={resolved}). Do not trust this as cleared.")


@_core._tool()
def kiln_configs_quarantine_clear(confirm: bool = False, host: Optional[str] = None) -> str:
    """Clear a quarantined kiln_configs store (POST
    /api/kiln_configs/quarantine_clear, kiln_cfg_http.c's quarantine_clear_
    post_handler(), ROUTE_TIER_ADMIN) -- the one way out short of a full
    ``factory_reset(scope=KILN)`` erasing the whole kiln_nvs partition. The
    store quarantines itself on boot (kiln_cfg_store.c's set_quarantine())
    if the persisted blob is the wrong size for any known schema version;
    every save/clone/rename/delete on saved kiln configs is refused until
    this runs. The board's LIVE zones config is unaffected either way --
    only the saved-slots store is quarantined.

    Always checks status FIRST: fetches GET /api/kiln_configs (a sanity
    read -- confirms the board answers at all) and then the quarantine
    status itself via a non-mutating probe (kiln_configs_quarantine_http_
    client.get_quarantine_status(), which POSTs with no confirm field --
    the route's own ordering answers that without changing anything). If
    the store is not quarantined, this returns that and does nothing else.

    REFUSES UNLESS ``confirm=True`` -- without it, this is a dry run: it
    reports whether the store is quarantined and what it WOULD clear, but
    sends no confirming POST. Same rule crash_report_ack()/safety_set_rate_
    guard() already use.

    With ``confirm=True``, POSTs confirm=1 (discarding whatever kiln config
    bytes could not be read and starting a fresh, empty store), then
    re-probes quarantine status and FAILS LOUDLY (does not report success)
    if the store still reads quarantined afterward -- same "do not trust an
    ok:true POST reply alone" rule this codebase's other write tools use
    (see CLAUDE.md's boot_guard write-lies section).

    Host is auto-resolved the same way get_heap_status()/crash_report_ack()
    do; pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as crash_report_ack()
    from . import kiln_configs_quarantine_http_client as qc

    resolved = _ota_resolve_host(host)
    try:
        listing = qc.get_kiln_configs_list(resolved)
    except qc.KilnConfigsQuarantineHttpError as exc:
        return f"error: could not read GET /api/kiln_configs (host={resolved}): {exc}"
    count = len(listing.get("configs", [])) if isinstance(listing, dict) else 0

    try:
        quarantined, detail = qc.get_quarantine_status(resolved)
    except qc.KilnConfigsQuarantineHttpError as exc:
        return f"error: could not read quarantine status (host={resolved}): {exc}"

    if not quarantined:
        return (f"not quarantined, nothing to do -- {count} saved config(s) visible "
                f"(host={resolved}): {detail}")

    if confirm is not True:
        return (f"DRY RUN (pass confirm=True to actually clear) -- store IS quarantined: "
                f"{detail} ({count} saved config(s) currently visible, host={resolved})")

    try:
        result = qc.post_quarantine_clear(resolved)
    except qc.KilnConfigsQuarantineHttpError as exc:
        return f"error clearing quarantine over HTTP (host={resolved}): {exc}"

    try:
        after_quarantined, after_detail = qc.get_quarantine_status(resolved)
    except qc.KilnConfigsQuarantineHttpError as exc:
        return (f"error: POST /api/kiln_configs/quarantine_clear returned {result!r}, but the "
                f"confirming re-probe failed (host={resolved}): {exc} -- quarantine state "
                f"UNKNOWN, re-check before trusting this")

    if not after_quarantined:
        return f"ok - quarantine cleared and confirmed by read-back: {result!r} (host={resolved})"
    return (f"FAILED: POST /api/kiln_configs/quarantine_clear returned {result!r}, but the "
            f"re-probed store still reads quarantined ({after_detail}) -- host={resolved}. Do "
            f"not trust this as cleared.")


@_core._tool()
def kiln_config_apply(id: int, confirm: bool = False, ack_hardware_differs: bool = False,
                       host: Optional[str] = None) -> str:
    """Apply a saved kiln config by id (POST /api/kiln_configs/apply,
    kiln_cfg_http.c's apply_post_handler(), ROUTE_TIER_ADMIN) -- the same
    route the kiln_configs page's own "Apply" button drives, and the only
    PcTools/MCP path to it before this tool: web_commission_row.py's
    `_apply_kiln_config` exercises it through a raw web session for its own
    commissioning test, not as a general-purpose client.

    REFUSES UNLESS ``confirm=True`` -- without it, this is a dry run that
    reports the id and does nothing else. Same rule kiln_configs_quarantine_
    clear()/crash_report_ack() already use.

    The apply is asynchronous: a successful POST returns 202 "running", not
    "done" -- kiln_cfg_swap_apply() is a 60+ round-trip UART transaction
    plus a flash write, dispatched to a worker rather than run on the httpd
    stack. This tool polls GET /api/kiln_configs/apply_status to a terminal
    state (done_ok/done_failed, up to 60s) before reporting a result, and
    surfaces `diverged=true` loudly if the board reports the swap left
    zone/guard settings in a mismatched state.

    ``ack_hardware_differs`` controls the ``X-Kiln-Ack-Hardware-Differs``
    header (kiln_cfg_store_slot_hardware_differs(), kiln_cfg_http.c around
    line 361/kiln_cfg_store.c around line 1466/1480): a stored config whose
    hardware shape (channel count, CT topology, etc.) differs from the
    board's own is refused with 428 "Precondition Required" and the board's
    own explanation UNLESS this header is set. (A 428 is not always that
    refusal -- the interlock's own "no safety processor" refusal uses 428
    too, and this tool tells the two apart by the body rather than assuming.)
    It defaults to False so a
    hardware mismatch is never silently masked -- on a 428 without it, this
    tool returns the board's message verbatim plus a hint to retry with
    ``ack_hardware_differs=True`` once that message has actually been read
    and understood, never automatically.

    Host is auto-resolved the same way get_heap_status()/crash_report_ack()
    do; pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as crash_report_ack()
    from . import kiln_configs_apply_http_client as ac

    resolved = _ota_resolve_host(host)

    if confirm is not True:
        return (f"DRY RUN (pass confirm=True to actually apply) -- would POST "
                f"/api/kiln_configs/apply for id={id} (ack_hardware_differs={ack_hardware_differs}, "
                f"host={resolved})")

    try:
        status, body = ac.post_apply(resolved, id, ack_hardware_differs=ack_hardware_differs)
    except ac.KilnConfigsApplyHttpError as exc:
        return f"error: could not reach {resolved} for POST /api/kiln_configs/apply: {exc}"

    if status == 428:
        # Two different firmware paths answer 428 here, and only ONE of them
        # is answerable by ack_hardware_differs: apply_post_handler() runs
        # ota_http_check_interlocks() first, and ota_http_send_interlock_
        # refusal() returns 428 for OTA_INTERLOCK_REFUSED_NEEDS_ACK (no
        # safety processor), which wants X-Ota-Ack-No-Safety instead -- a
        # header this tool deliberately never sends. Naming the wrong one
        # would send the operator into an endless identical retry.
        if ac.is_hardware_differs_body(body):
            return (f"refused (428, hardware differs) for id={id}: {body} -- retry with "
                    f"ack_hardware_differs=True only after reading and understanding this message "
                    f"(host={resolved})")
        return (f"refused (428, interlock precondition) for id={id}: {body} -- this is NOT the "
                f"hardware-differs refusal and ack_hardware_differs=True will not clear it; it "
                f"needs the safety-processor acknowledgement (X-Ota-Ack-No-Safety), which this "
                f"tool does not send. Resolve the named precondition on the board first "
                f"(host={resolved})")
    if status == 409:
        from . import zones_http_client  # local import: avoid a module-load-order cycle, same convention as the other local imports in this function
        if zones_http_client.is_system_mode_gate_refusal(body):
            return (f"refused: system_mode_gate refused this apply (HTTP 409) for id={id}: {body} -- "
                    f"a firing or autotune run is active; applying a saved kiln config is not "
                    f"available until it ends. Distinct from this route's own 428 refusals above "
                    f"(host={resolved})")
        return f"error: POST /api/kiln_configs/apply returned unexpected HTTP 409: {body} (host={resolved})"
    if status == 404:
        return f"error: no such kiln config id={id} (404): {body} (host={resolved})"
    if status not in (200, 202):
        return f"error: POST /api/kiln_configs/apply returned unexpected HTTP {status}: {body} (host={resolved})"

    try:
        final = ac.poll_apply_status(resolved)
    except ac.KilnConfigsApplyHttpError as exc:
        return (f"error: apply for id={id} was accepted ({status}: {body}), but polling "
                f"/api/kiln_configs/apply_status failed (host={resolved}): {exc} -- outcome UNKNOWN")

    state = final.get("state")
    diverged = final.get("diverged")
    reason = final.get("reason")
    if state == "done_ok" and not diverged:
        return f"ok - applied id={id}, confirmed by apply_status (host={resolved})"
    if diverged:
        # 2026-09-22 review fix: `diverged` alone does not mean heat was
        # disabled -- only kiln_cfg_swap.c's ceiling-latch branch does that,
        # not its four rollback-failure branches. `reason` is the honest
        # source (docs/audits/kiln_config_self_apply_diverged_2026-09-22.md
        # sec 4).
        return (f"FAILED: apply for id={id} left the board DIVERGED (state={state!r}, "
                f"reason={reason!r}) -- board alarmed, do not trust config state; see reason "
                f"for whether heat was also disabled (host={resolved})")
    if state == "done_failed":
        return f"FAILED: apply for id={id} refused/failed: {reason!r} (host={resolved})"
    return (f"UNKNOWN: apply for id={id} did not reach a terminal state within the poll window "
            f"(state={state!r}, reason={reason!r}) -- re-check /api/kiln_configs/apply_status by "
            f"hand (host={resolved})")


@_core._tool()
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


#: backup_export()'s default output directory, anchored at the repo root
#: (this file is tools/PcTools/src/kilnctrl/, four levels below it) rather
#: than the server process's cwd. Gitignored.
_BACKUP_EXPORT_DEFAULT_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))), "logs", "backup_export")


@_core._tool()
def backup_export(out_path: Optional[str] = None, host: Optional[str] = None) -> str:
    """READ-ONLY: fetch the board's settings/profile backup (GET
    /api/backup/export, backup_export.c's backup_export_get_handler(),
    ROUTE_TIER_ADMIN) and write the raw JSON to a local file. Pure GET, no
    side effects on the board.

    Default `out_path` is ``<repo root>/logs/backup_export/kilnctl_backup_<UTC
    timestamp>.json`` (anchored at the repo root, not the server's cwd; ``logs/backup_export/`` is
    gitignored, same convention as ``logs/bench_test/``) -- pass an
    explicit path to save elsewhere.

    Reports the byte size written, the document's ``version`` field
    (BACKUP_FORMAT_VERSION on the board), and a one-line-per-section count
    (profiles/zones/kiln_configs -- whichever top-level array keys are
    present, with how many entries each holds).

    Also scans the raw response text for Wi-Fi/password-shaped substrings
    ("wifi", "ssid", "password", "passphrase", "psk") and reports ONLY
    whether any were found, never the matched text or its value --
    backup_http.h's own header comment says this document should never
    carry Wi-Fi credentials or the web admin password by design (restoring
    a backup onto a different board must never silently change what
    network it joins or overwrite its own web auth), so a `True` here means
    "stop and look by hand," not routine data.

    Host is auto-resolved the same way get_readiness()/get_heap_status() do.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()
    from . import backup_export_http_client

    resolved = _ota_resolve_host(host)
    try:
        raw_text, parsed = backup_export_http_client.get_export(resolved)
    except backup_export_http_client.BackupExportHttpError as exc:
        return f"error: could not fetch GET /api/backup/export (host={resolved}): {exc}"

    if out_path is None:
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        out_path = os.path.join(_BACKUP_EXPORT_DEFAULT_DIR, f"kilnctl_backup_{stamp}.json")
    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    data_bytes = raw_text.encode("utf-8")
    with open(out_path, "wb") as f:
        f.write(data_bytes)

    section_lines = []
    for key, value in parsed.items():
        if key in ("kind", "version"):
            continue
        if isinstance(value, list):
            section_lines.append(f"{key}: {len(value)} entries")
        else:
            section_lines.append(f"{key}: present (not a list)")

    sensitive = backup_export_http_client.scan_for_sensitive_fields(raw_text)

    lines = [
        f"host={resolved}",
        f"wrote {len(data_bytes)} bytes to {out_path}",
        f"kind={parsed.get('kind')!r} version={parsed.get('version')!r}",
    ]
    lines.extend(f"  {s}" for s in section_lines)
    lines.append(f"contains wifi/password-shaped fields: {sensitive}")
    return "\n".join(lines)


@_core._tool()
def backup_import(
    path: str,
    confirm: bool = False,
    mode: str = "merge",
    dry_run: bool = False,
    ack_delete_count: Optional[int] = None,
    ack_no_safety: bool = False,
    host: Optional[str] = None,
) -> str:
    """Restore a settings/profile backup from a local file (POST
    /api/backup/import, backup_import.c's backup_import_post_handler(),
    ROUTE_TIER_ADMIN). `path` is a local file path -- e.g. one written by
    ``backup_export()`` -- read and sent verbatim as the request body.

    UNLIKE ``kiln_config_apply()``, this route is SYNCHRONOUS: there is no
    job id and no status-poll route. The board validates the whole document
    (a two-pass validate-then-commit parser -- nothing is written unless
    every profile/zone/kiln_config entry validates first) and either
    commits or refuses within the one POST/response round trip. This tool
    reports the same elapsed wall time for "the POST" and "the full job"
    for that reason -- there is only one interval to measure, and this
    docstring/result says so rather than implying a poll that does not
    exist for this route.

    2026-09-28 (docs/HTTP_POST_OWNER_MIGRATION.md slice A4): the slow
    tail now runs off esp_http_server's shared httpd_worker task via
    http_async_job.c, so a long restore no longer stalls every OTHER
    request while it runs -- but the wire contract above (one POST, one
    response, no job id, no poll) did not change, and this call can still
    legitimately take up to BACKUP_IMPORT_HTTP_TIMEOUT_S
    (backup_import_http_client.py, 90s as of this migration). If this call
    raises/reports a transport-side failure (a timeout or a dropped
    connection), that means only that no response arrived in time -- a
    bench A4 run (2026-09-28) hit exactly this and the import had, in fact,
    already committed. THIS TOOL DOES NOT RETRY, and a caller should not
    either: re-POSTing an import whose outcome is unknown risks re-applying
    it. Call ``get_readiness()``/``control_get_zones()`` (or re-run this
    same tool with ``dry_run=True``, which is always safe -- it never
    writes) to find out what the board actually has before deciding what to
    do next.

    REFUSES UNLESS ``confirm is True`` (exactly `True`) -- without it,
    nothing is read off disk and nothing is POSTed; the only board access
    is the read-only GET /api/readiness below, and the tool reports what it
    WOULD do and stops. This holds even for ``dry_run=True``: the board's
    own dry-run mode computes and returns a plan without writing anything,
    but this tool still will not POST it without an explicit confirm, so a
    caller cannot forget it once and get used to dry-run POSTs going
    through silently.

    Always fetches GET /api/readiness FIRST (before touching the file or
    the board) and reports its summary; re-fetches it AFTER the POST and
    reports that too, so a caller can see what changed. A readiness fetch
    failure on either end is reported but does not by itself refuse the
    restore (readiness is diagnostic context here, not a precondition the
    firmware itself checks for this route).

    A non-2xx response is classified and reported by REFUSAL CATEGORY, not
    just status code, since a plain 409 is ambiguous on this route
    (backup_import_http_client.classify_refusal()):
      * "mode_gate" (409) -- a firing or autotune run is active; ALL zone/
        profile/kiln_config writes are refused while one is running,
        PAUSED included.
      * "async_busy" (409) -- an unrelated commissioning job (ct_auto_zero)
        is mid-commit; retry once it finishes.
      * "interlock" (409) -- an ordinary OTA-interlock precondition (heater
        on, zone over temperature, another update in flight, etc).
      * "interlock_needs_ack" (428) -- the safety link is down; retry with
        ``ack_no_safety=True`` if that is actually intended.
      * "validation" (400) -- the document was refused before anything was
        written (the board's own error message is included verbatim).
      * "out_of_memory" (500, body exactly "out of memory") -- a buffer
        allocation failed before anything was parsed or written.
      * "partial_write" (any other 500) -- FAILS LOUD: a partial write landed
        (kiln_configs[] committed before profiles/zones failed) -- this is
        reported as a failure, never papered over as a partial success.

    `mode` is "merge" (default) or "mirror" (mirror also deletes
    unmatched kiln config slots on the board -- see backup_import.c's own
    comment; `ack_delete_count` must exactly match the number of slots a
    non-dry-run mirror restore would delete, or it refuses).

    Never prints, logs, or echoes a credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()
    from . import backup_import_http_client
    from . import readiness_http_client

    resolved = _ota_resolve_host(host)

    def _readiness_summary(label: str) -> str:
        try:
            data = readiness_http_client.get_readiness(resolved)
        except readiness_http_client.ReadinessHttpError as exc:
            return f"{label}: could not read GET /api/readiness: {exc}"
        items = data.get("items", [])
        ok = sum(1 for i in items if i.get("status") == "ok")
        not_done = sum(1 for i in items if i.get("status") == "not_done")
        other = len(items) - ok - not_done
        return f"{label}: {ok} ok, {not_done} not_done, {other} other ({len(items)} total)"

    before_readiness = _readiness_summary("readiness before")

    if confirm is not True:
        return (
            f"DRY RUN -- pass confirm=True (exactly) to actually POST {path} to "
            f"/api/backup/import (host={resolved}, mode={mode!r}, dry_run={dry_run}). "
            f"{before_readiness}"
        )

    try:
        with open(path, "r", encoding="utf-8") as f:
            body_text = f.read()
    except OSError as exc:
        return f"error: could not read {path}: {exc}"

    t0 = time.monotonic()
    try:
        status, resp_text = backup_import_http_client.post_import(
            resolved, body_text, mode=mode, dry_run=dry_run,
            ack_delete_count=ack_delete_count, ack_no_safety=ack_no_safety)
    except backup_import_http_client.BackupImportHttpError as exc:
        return (f"error: POST /api/backup/import failed transport-side (host={resolved}): {exc} -- "
                f"outcome UNKNOWN: the import MAY HAVE COMMITTED (a 2026-09-28 bench run timed out "
                f"and had committed). Do not retry blindly; check get_readiness()/control_get_zones() "
                f"or re-run with dry_run=True before deciding.")
    elapsed_s = time.monotonic() - t0

    after_readiness = _readiness_summary("readiness after")

    if 200 <= status < 300:
        if dry_run:
            return (
                f"ok - dry run only, nothing written. plan:\n{resp_text}\n"
                f"(POST elapsed {elapsed_s:.2f}s; this route is synchronous, so the full job "
                f"took the same {elapsed_s:.2f}s) (host={resolved})\n{before_readiness}\n"
                f"{after_readiness}"
            )
        return (
            f"ok - restored. (POST elapsed {elapsed_s:.2f}s; this route is synchronous, so the "
            f"full job took the same {elapsed_s:.2f}s) (host={resolved})\n{before_readiness}\n"
            f"{after_readiness}"
        )

    category = backup_import_http_client.classify_refusal(status, resp_text)
    if category == backup_import_http_client.REFUSAL_PARTIAL_WRITE:
        return (
            f"FAILED: HTTP {status} -- a PARTIAL write landed before the restore failed: "
            f"{resp_text} (POST elapsed {elapsed_s:.2f}s) (host={resolved})\n{before_readiness}\n"
            f"{after_readiness}"
        )
    return (
        f"refused: HTTP {status} [{category}]: {resp_text} (POST elapsed {elapsed_s:.2f}s) "
        f"(host={resolved})\n{before_readiness}\n{after_readiness}"
    )


@_core._tool()
def boot_guard_get(host: Optional[str] = None) -> str:
    """READ-ONLY: fetch boot_guard's recovery-mode counter (GET
    /api/boot_guard, App/drivers/http/ota_http_recovery.c's
    ota_boot_guard_status_get_handler()) -- the same data
    flash_firmware()'s post-flash boot_guard_reset step reads internally,
    now available as its own call so the counter can be checked without a
    flash in flight (previously only observable "by hand", per the
    2026-09-22 bench finding that led to this tool).

    Unauthenticated when web auth is off; 401s and requires an admin
    session once web auth is on -- this goes through the same
    ota_http_client.get_boot_guard_status() helper flash_firmware() already
    calls, which itself uses http_auth.urlopen(), the same ADMIN-session
    seam every other admin-tier tool in this package uses. Pure GET, no
    side effects, refuses nothing, and reports no credentials.

    Returns ``{"boot_count": int, "recovery_mode": bool, "persisted_count":
    int|None}`` rendered as one short text block plus a one-line
    interpretation. ``boot_count`` is fixed for the life of the current
    boot (set once at boot_guard_init()); ``persisted_count`` is a live
    re-read of what is actually in NVS right now (0 covers both "really
    cleared" and "nothing valid there" -- see
    boot_guard_get_persisted_count()'s doc comment in boot_guard.h), so it
    is the field that actually moves right after a boot_guard_reset call.
    Older firmware that predates this field omits it; this renders as
    ``persisted_count=None`` rather than an error. See CLAUDE.md's
    boot_guard section and docs/audits/boot_guard_post_flash_recovery_
    footgun_2026-09-08.md for why this counter and this route exist.

    Host is auto-resolved the same way get_heap_status()/the OTA/control
    tools do; pass `host` explicitly for kilnctl.local or a board reachable
    only from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()
    from . import ota_http_client as ota_http

    resolved = _ota_resolve_host(host)
    try:
        data = ota_http.get_boot_guard_status(resolved)
    except ota_http.OtaHttpError as exc:
        return f"error reading boot_guard over HTTP (host={resolved}): {exc}"

    boot_count = data.get("boot_count")
    recovery_mode = data.get("recovery_mode")
    persisted_count = data.get("persisted_count")
    return (
        f"host={resolved}\n"
        f"boot_count={boot_count!r}\n"
        f"recovery_mode={recovery_mode!r}\n"
        f"persisted_count={persisted_count!r}\n"
        f"summary: counter {boot_count!r}, recovery_mode {recovery_mode!r}, "
        f"persisted {persisted_count!r}"
    )


@_core._tool()
def nvs_list_keys(partition: str, namespace: str, host: Optional[str] = None) -> str:
    """READ-ONLY: list the key NAMES AND TYPES (never values, never blobs)
    in one NVS namespace, over GET /api/nvs/keys?partition=<partition>&
    namespace=<namespace> (diagnostics_http.c's nvs_keys_get_handler()).

    Added 2026-09-21 for the bench-side half of the Wi-Fi factory_reset
    driver-storage audit
    (docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md): confirms
    esp_wifi_restore() actually empties the driver's own `nvs.net80211`
    namespace in the default `nvs` partition after
    factory_reset(scope=wifi) -- call this before and after that reset and
    compare the key lists, instead of a JTAG memory read.

    Refuses the `kiln_auth` namespace locally (no request made) -- that
    namespace holds the administrator credential record, and the board's
    own handler refuses it too (403). No other namespace is special-cased.

    Renders one line per key as ``key (type)``, sorted, followed by a
    count. An empty namespace renders as a bare count of 0 -- exactly what
    "successfully emptied" looks like.

    Host is auto-resolved the same way get_readiness()/get_heap_status() do.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()
    from . import nvs_keys_http_client

    resolved = _ota_resolve_host(host)
    try:
        data = nvs_keys_http_client.get_nvs_keys(resolved, partition, namespace)
    except nvs_keys_http_client.NvsKeysHttpError as exc:
        return (f"error reading NVS keys over HTTP (host={resolved}, partition={partition!r}, "
                f"namespace={namespace!r}): {exc}")

    keys = data.get("keys", [])
    lines = [f"host={resolved} partition={data.get('partition', partition)!r} "
             f"namespace={data.get('namespace', namespace)!r}"]
    for entry in sorted(keys, key=lambda e: e.get("key", "")):
        lines.append(f"{entry.get('key', '?')} ({entry.get('type', '?')})")
    lines.append(f"summary: {len(keys)} key(s)")
    return "\n".join(lines)


@_core._tool()
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


@_core._tool()
def get_cfgfs_status(host: Optional[str] = None) -> str:
    """Report the `cfg` LittleFS partition's live state, over HTTP GET
    /api/cfgfs (diagnostics_http.c: cfgfs_status_get_handler()).

    docs/FILESYSTEM_USER_DATA.md's user-data-on-a-filesystem migration
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
    list of items docs/FILESYSTEM_USER_DATA.md section 5 has not yet
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

    subdirs = cfgfs.get("subdirs") or []
    for sd in subdirs:
        unknown = sd.get("unknown_size") or 0
        note = f" ({unknown} of unknown size)" if unknown else ""
        lines.append(
            f"  {sd.get('name')}/: {sd.get('file_count')} file(s), {sd.get('size_bytes')} B{note}"
        )

    dual = cfgfs.get("dual_write") or {}
    items = dual.get("items") or []
    lines.append(f"dual_write: write_mode={dual.get('write_mode')} items={len(items)}")
    for it in items:
        flags = []
        if it.get("diverged"):
            flags.append("!!! DIVERGED -- a prior file write failed, only NVS advanced")
        if it.get("nvs_stale"):
            flags.append("nvs_stale (file is newer than NVS)")
        if it.get("migration_deferred"):
            flags.append("migration_deferred")
        if not it.get("file_backed"):
            flags.append("not file-backed yet")
        tail = f" [{'; '.join(flags)}]" if flags else ""
        lines.append(
            f"  dual_write.{it.get('name')}: file_rev={it.get('file_rev')} "
            f"nvs_rev={it.get('nvs_rev')}{tail}"
        )
    nvs_only = dual.get("nvs_only") or []
    if nvs_only:
        lines.append(f"still NVS-only: {', '.join(nvs_only)}")
    nvs_permanent = dual.get("nvs_permanent") or []
    if nvs_permanent:
        lines.append(f"NVS by design: {', '.join(nvs_permanent)}")

    return "\n".join(lines)


@_core._tool()
def cfgfs_format(confirm: bool = False, host: Optional[str] = None, force_healthy: bool = False) -> str:
    """Confirm-and-format the `cfg` LittleFS partition -- POST
    /api/cfgfs/format_confirm (cfg_fs_format_http.c's format_confirm_post_
    handler(), ROUTE_TIER_ADMIN). This is the operator confirmation
    cfg_fs_mount.c's auto-format gate waits for once it decides, at boot,
    that formatting the `cfg` partition would silently discard data it
    cannot otherwise recover (s_format_confirmation_pending -- see
    cfg_fs_mount.h/.c and App/main_boot_early.c's boot-time log line
    pointing an operator at this exact endpoint).

    DESTRUCTIVE: on success this ERASES EVERY FILE cfg_fs holds (zones,
    prefs, profiles, ramp_assist, tz, relay_cycles, adaptive_tune,
    firing_stats -- whatever get_cfgfs_status() currently lists) and remounts
    an empty filesystem. Since the NVS dual-write close the `cfg` partition is the ONLY
    copy of zones/prefs/profiles, so erasing it loses them for good (export a backup first).
    This is a `cfg`-partition-only action, not a factory_reset(scope=KILN).

    HEALTHY-CFG GUARD: since the NVS dual-write close, a mounted cfg partition is
    the ONLY copy of zones/profiles/preferences, so the firmware answers 409
    ("cfg is mounted and healthy ...") to a format of a healthy partition. This
    tool reports that as a refusal with the firmware's message. Only
    ``force_healthy=True`` (default False, still requires ``confirm=True``) sends
    the explicit override ``?force_healthy=1``; use it only when you really mean
    to erase a working cfg partition. A partition that is not mounted (the
    needs-format case) needs no override.

    Always reads GET /api/cfgfs FIRST and reports the current file count
    (never guesses). REFUSES UNLESS ``confirm=True`` -- without it, this is a
    dry run: it reports the current file count and that it would format, but
    sends no POST at all. Same rule crash_report_ack()/kiln_configs_
    quarantine_clear()/safety_set_rate_guard() already use.

    With ``confirm=True``, POSTs through the sanctioned ota_http_client/
    http_auth seam -- ROUTE_TIER_ADMIN (the admin session) is now the only
    auth this route requires, same as POST /api/factory_reset, which used to
    share its "factory-reset" HMAC context with this route before the
    AP-password HMAC challenge/response scheme was retired 2026-09-29
    (WEB_AUTH_PLAN.md item 2b).

    After the POST, re-reads GET /api/cfgfs and reports the after-state file
    count (should read 0, an empty freshly-formatted filesystem) so a caller
    never has to trust the POST's own plain-text response alone.

    Host is auto-resolved the same way get_cfgfs_status()/crash_report_ack()
    do; pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from . import ota_http_client as ota_http
    from .mcp_server_ota import _ota_resolve_host  # local import: avoid circular imports, same convention as kiln_configs_quarantine_clear()

    resolved = _ota_resolve_host(host)
    try:
        before = dashboard_http_client.get_cfgfs_status(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: could not read GET /api/cfgfs (host={resolved}): {exc}"
    before_count = before.get("file_count")

    if confirm is not True:
        healthy_note = ""
        if before.get("mounted") is True and not force_healthy:
            healthy_note = ("; cfg is mounted and healthy, so the firmware would refuse (409) unless "
                            "force_healthy=True is also passed")
        return (f"DRY RUN (pass confirm=True to actually format) -- cfg partition currently holds "
                f"{before_count} file(s) (host={resolved}); formatting would erase all of them"
                f"{healthy_note}")

    try:
        if force_healthy:
            result = ota_http.format_cfgfs(resolved, force_healthy=True)
        else:
            result = ota_http.format_cfgfs(resolved)
    except ota_http.OtaHttpError as exc:
        from . import zones_http_client  # local import: avoid a module-load-order cycle, same convention as the other local imports in this function
        if exc.status == 409 and zones_http_client.is_system_mode_gate_refusal(exc.detail):
            return (f"refused: system_mode_gate refused this format (HTTP 409): {exc.detail} -- "
                    f"a firing or autotune run is active; cfgfs format is not available until it "
                    f"ends. Distinct from OTA's own 428 interlock (host={resolved}, "
                    f"before file_count={before_count})")
        if exc.status == 409:
            return (f"refused: firmware answered HTTP 409: {exc.detail} (host={resolved}, "
                    f"before file_count={before_count}); nothing was formatted")
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved}, before file_count={before_count})"

    try:
        after = dashboard_http_client.get_cfgfs_status(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return (f"WARNING: POST /api/cfgfs/format_confirm returned {result!r}, but the confirming "
                f"re-read failed (host={resolved}): {exc} -- after-state UNKNOWN, re-check "
                f"before trusting this")
    after_count = after.get("file_count")
    after_mounted = after.get("mounted")

    if after_mounted is not True or after_count != 0:
        return (f"error: POST /api/cfgfs/format_confirm returned {result!r}, but the re-read "
                f"(host={resolved}) shows mounted={after_mounted!r}, file_count={after_count!r} "
                f"-- expected mounted=True, file_count=0; before file_count={before_count}")

    return (f"ok - cfg partition formatted (host={resolved}): before file_count={before_count}, "
            f"after file_count={after_count}; board detail: {result.get('detail')!r}")


@_core._tool()
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

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
