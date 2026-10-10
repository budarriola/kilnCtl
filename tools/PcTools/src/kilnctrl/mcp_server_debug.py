"""generic debug (OpenOCD program/reset/halt/step/memory/registers).

Part of the mcp_server.py split (pure refactor) -- moved verbatim, no
logic changes. See mcp_server.py's module docstring for the overall map.
"""
from __future__ import annotations

import re
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

from . import actions, config_presets, debug_probe, devices, elf_archive, mcp_facade, openocd_util, pico_gpio_probe, reset_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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


# Cap on how much raw OpenOCD output gets written to the session log per
# failed invocation -- generous enough to hold a whole failed reset/program
# transcript, small enough that a wedged board spamming retries can't blow
# the log file up.
_OPENOCD_LOG_TRUNCATE_BYTES = 16 * 1024

# Lines matching one of these (checked in order) are the ones worth quoting
# on their own as "the decisive line" in a tool's returned error -- these are
# the substrings OpenOCD/this codebase's own checks (see
# openocd_util.run_openocd's `ok = ...` line) treat as failure markers, plus
# a couple of common Tcl-level failures seen in practice (invalid command
# name against a not-yet-inited target, "Polling failed" for a probe/board
# that vanished mid-session).
_DECISIVE_MARKERS = ("Verify Failed", "Error:", "invalid command name", "Polling failed")


def _decisive_openocd_line(output: str) -> str:
    """Picks the one line out of a (possibly long) OpenOCD transcript that
    most likely explains a failure, for use in a tool's returned `error`
    message. Falls back to the last non-blank line, then a fixed string, so
    this never returns an empty message."""
    lines = [ln.strip() for ln in (output or "").splitlines() if ln.strip()]
    for marker in _DECISIVE_MARKERS:
        for line in lines:
            if marker in line:
                return line
    return lines[-1] if lines else "(no OpenOCD output captured)"


def _log_openocd_result(op: str, ok: bool, output: str) -> None:
    """Persists an OpenOCD invocation's outcome to the session log.

    On success this is a single short line -- ``ok=True`` alone was already
    logged by most callers before this helper existed, and a healthy board
    doing routine debug_reset/debug_halt calls should not spam the session
    log with full OpenOCD transcripts.

    On failure, the FULL stdout+stderr capture (``output``, as returned by
    ``openocd_util.run_openocd`` via debug_probe.py) is written to the
    session log, truncated to ``_OPENOCD_LOG_TRUNCATE_BYTES`` -- this is the
    only place that transcript is ever persisted; the MCP tool's return value
    is not logged anywhere by itself, so without this a failed reset/program/
    halt could only be diagnosed after the fact by reproducing it again.
    """
    if ok:
        _srv._session_log.warning("%s: ok", op)
        return
    text = output or "(no output captured)"
    if len(text) > _OPENOCD_LOG_TRUNCATE_BYTES:
        text = text[-_OPENOCD_LOG_TRUNCATE_BYTES:]
        text = "...[truncated]...\n" + text
    _srv._session_log.warning("%s: FAILED\n%s", op, text)


def _openocd_error_message(action_desc: str, output: str) -> str:
    """Builds the string an MCP tool returns for a failed OpenOCD call:
    the decisive line up front (so it's the first thing read), then the last
    ~20 lines of raw output for context."""
    decisive = _decisive_openocd_line(output)
    tail = "\n".join((output or "").strip().splitlines()[-20:])
    return f"error: {action_desc}: {decisive}\n\n{tail}"


# ---------------------------------------------------------------------------
# Generic debug (OpenOCD program/reset/halt/step/memory/registers), both
# peers. See debug_probe.py's module docstring for the full design rationale
# (tools/PcTools/TODO.md "One substrate covers both: OpenOCD") and the guard
# rails that are and are not implemented. This section only adds the MCP
# tool wrappers + the policy pieces debug_probe.py deliberately doesn't know
# about (session logging, the ESP-halt-during-profile guard, the
# write-requires-confirm gate).
# ---------------------------------------------------------------------------
@_core._tool()
def set_openocd_path(path: str) -> str:
    """Sets a persisted override for openocd.exe, for machines where
    autodetection (settings.json override -> OPENOCD_EXE env var ->
    ~/.espressif/tools/openocd-esp32/ -> C:\\Espressif\\...) doesn't find it.

    Only needed if get_openocd_status() reports "not found". The path is
    validated (must exist and be a file) then persisted to settings.json and
    used first on every subsequent flash_firmware()/debug_*() call."""
    if not path or not os.path.isfile(path):
        raise ValueError(f"path does not exist or is not a file: {path!r}")
    settings.set_openocd_path(path)
    return f"openocd path set to {path} (persisted in settings.json)"


@_core._tool()
def get_openocd_status() -> str:
    """Reports the openocd.exe path that will actually be used, where it came
    from (explicit override / OPENOCD_EXE env var / autodetection / not
    found), and which peers' default ELF build outputs currently exist on
    disk -- a quick way to confirm what debug_program() will do before
    calling it."""
    override = settings.get_openocd_path()
    env_path = os.environ.get("OPENOCD_EXE")
    resolved = openocd_util._find_openocd_exe()

    if resolved is None:
        source_line = "openocd.exe: not found -- set one with set_openocd_path()"
    elif override and os.path.isfile(override) and resolved == override:
        source_line = f"openocd.exe: {resolved} (source: settings.json override)"
    elif env_path and os.path.isfile(env_path) and resolved == env_path:
        source_line = f"openocd.exe: {resolved} (source: OPENOCD_EXE env var)"
    else:
        source_line = f"openocd.exe: {resolved} (source: autodetected)"

    lines = [source_line]
    for peer, label, elf_fn in (
        (debug_probe.PEER_ESP, "esp (KilnFW)", debug_probe._kiln_fw_elf),
        (debug_probe.PEER_PICO, "pico (SaftyFW)", debug_probe._safty_fw_elf),
    ):
        elf = elf_fn()
        present = "found" if os.path.isfile(elf) else "MISSING -- pass elf_path explicitly or build first"
        lines.append(f"{label} default elf: {elf} ({present})")
    return "\n".join(lines)


def _archive_flashed_safty_elf(elf_path: str, explicit_elf_path: Optional[str] = None) -> str:
    """Best-effort SaftyFW counterpart to mcp_server_flash._archive_flashed_elf
    -- SaftyFW has no archive_elf.cmake step at all (KilnFW's exists, SaftyFW's
    does not), so this is the ONLY thing that archives a flashed Pico ELF.

    `elf_path` must be the ELF that was ACTUALLY flashed -- the caller's
    resolved path (whatever debug_probe.program() used: the caller's own
    `elf_path` argument if one was given, else the peer's default build
    output), never re-derived independently here. 2026-09-21 bug: this used
    to ignore the caller's `elf_path` entirely and always archive
    debug_probe._safty_fw_elf() (the default main-tree build path), so a
    `debug_program(peer="pico", elf_path=<worktree>/.../SaftyFW.elf)` flash
    from a clean worktree archived the wrong (default, possibly stale or
    absent) ELF instead of the one just flashed -- the correct ELF had to be
    hand-copied into the archive afterward.

    Second bug, same day: `archive_safty_elf()` reads
    `<safty_fw_root>/build/saftyfw_build_info.h` for the identity keying the
    manifest entry -- once `elf_path` was fixed to point at a worktree build,
    `safty_fw_root` still always came from `debug_probe._safty_fw_root()`
    (the main tree), so a worktree-built ELF got archived under the MAIN
    TREE's git identity instead of its own. `explicit_elf_path` is the
    caller's own (possibly None) `elf_path` argument to `debug_program()`;
    when it is given, the root is instead derived as the parent of its
    `build/` directory, so identity and binary come from the same tree. The
    default path (`explicit_elf_path is None`) still uses
    `debug_probe._safty_fw_root()`, since there the main tree IS what was
    flashed.

    Never raises into the caller -- see elf_archive.py's module docstring --
    but a failure is always surfaced in the returned string (never silent);
    see mcp_server_flash._archive_flashed_elf for the same pattern."""
    try:
        if explicit_elf_path is not None:
            resolved = os.path.abspath(explicit_elf_path)
            safty_fw_root = os.path.dirname(os.path.dirname(resolved))
        else:
            safty_fw_root = debug_probe._safty_fw_root()
        result = elf_archive.archive_safty_elf(elf_path, safty_fw_root, "debug_program(peer=pico)")
        return f"\n\nelf archived: {result.archived_path} (identity {result.identity})"
    except Exception as exc:  # noqa: BLE001 - archiving is a diagnostic convenience, never fail the flash over it
        _srv._session_log.warning("debug_program: safty elf archiving failed (non-fatal): %s", exc)
        return f"\n\nWARNING: elf archiving FAILED (flash itself succeeded): {exc}"


@_core._tool()
def debug_program(peer: str, elf_path: Optional[str] = None, confirm: bool = False, allow_stale: bool = False,
                  allow_running: bool = False) -> str:
    """Flashes an ELF to `peer` ("esp" or "pico") over OpenOCD and resets it.
    Writes flash on a live board -- refused unless `confirm=True` is passed
    explicitly (tools/PcTools/TODO.md's "flash writes require an explicit
    confirm" guard rail).

    Uses the peer's default build output (KilnFW/build/KilnCtrl.elf or
    SaftyFW/build/SaftyFW.elf) unless `elf_path` is given. For the ESP, prefer
    flash_firmware() instead -- it flashes the full three-image set
    (bootloader/partition-table/app) this tool does not; this generic path is
    for the RP2040, which ships one plain ELF with no bootloader.

    peer="esp" is REFUSED outright (before touching OpenOCD at all): this
    generic single-ELF program path reliably fails flash-bank detection/
    verify on this board (confirmed repeatedly) -- flash_firmware() is the
    sanctioned, working path for the ESP. This used to fail downstream after
    wasted time instead of refusing immediately; see CLAUDE.md's
    flash_firmware section. peer="pico" is unaffected -- the RP2040 has no
    bootloader/partition table to get wrong and this path works fine there.

    For peer="pico" (SaftyFW), same staleness refusal as flash_firmware(): the
    ELF's build-identity header (saftyfw_build_info.h) is checked against
    current HEAD and, if firmware/SaftyFW or firmware/CommonFW have
    uncommitted changes, against those changed files' mtimes -- see
    stale_check.py. Pass allow_stale=True to flash anyway. Only checked when
    using the peer's default ELF and default build-info location; an explicit
    elf_path bypasses the check (nothing to compare it against).

    Reprogramming the Pico resets it, so it is REFUSED while the ESP reports a
    running/paused profile or autotune. If the ESP state cannot be read
    (recovery image, bricked, link down) it proceeds with a loud WARNING in
    the result. allow_running=True (exactly True) overrides a confirmed run."""
    if peer == debug_probe.PEER_ESP:
        return (
            "error: debug_program(peer=\"esp\") is refused -- use flash_firmware(); "
            "debug_program esp path fails flash-bank detection on this board -- see CLAUDE.md"
        )

    if confirm is not True:
        return "error: flash write refused without confirm=True -- this writes flash on a live board"

    esp_warnings: list = []
    if allow_running is not True:
        refusal = _esp_profile_running_refusal("reprogram the Pico under", unreadable_warnings=esp_warnings)
        if refusal is not None:
            return refusal
    warn_prefix = "".join(w + chr(10) for w in esp_warnings)

    stale_prefix = ""
    if peer == debug_probe.PEER_PICO and elf_path is None:
        stale = stale_check.check_saftyfw_stale(debug_probe._safty_fw_root())
        if stale.stale:
            _srv._session_log.warning("debug_program: stale ELF detected for peer=%s: %s", peer, stale.reason)
            if not allow_stale:
                return (
                    "error: refusing to flash a stale ELF -- " + stale.reason + "\n\n"
                    "Rebuild with build_saftyfw_host_tests/the SaftyFW build first, or pass "
                    "allow_stale=True to flash this ELF anyway."
                )
            stale_prefix = f"WARNING: flashed a stale binary anyway ({stale.reason})\n"

    ok, output = debug_probe.program(peer, elf_path)
    _log_openocd_result(f"debug_program(peer={peer})", ok, output)
    if ok:
        note = ""
        if peer == debug_probe.PEER_PICO:
            note = (
                "\n\nNOTE: this reset the Pico. If the ESP was also reset around the "
                "same time (a dual reflash), expect a correct S6a (mainFault) trip "
                "only if the ESP is up and asserting mainFault while its safety link "
                "handshake is still coming up; if the ESP stays silent for more than "
                "120 s the Pico trips S6b (link dead, trip_reason 7) instead -- see "
                "docs/audits/s6a_startup_grace_revert_2026-09-07.md. Confirm the "
                "link is up (safety_get_status shows link up and FW_VERSION "
                "exchanged) before calling safety_clear_trip()."
            )
            resolved_elf = elf_path or debug_probe._safty_fw_elf()
            note += _archive_flashed_safty_elf(resolved_elf, explicit_elf_path=elf_path)
        return warn_prefix + stale_prefix + f"programmed {peer} OK, reset and running" + note
    return warn_prefix + _openocd_error_message(f"program failed for {peer}", output)


def _reset_marker_line(post_state: "Optional[dict]") -> str:
    """One line saying whether the reset script's markers were seen.

    Lets a reader tell the dark-board path (reset issued, core never back to
    `running`) from the healthy one without decoding the raw OpenOCD output.
    ``post_state`` is ``debug_probe.parse_post_reset()``'s dict, or None when
    the call was not an ESP run-mode reset (no markers exist then).
    """
    if post_state is None:
        return ""
    issued = "yes" if post_state["reset_issued"] else "NO (reset script never reached `reset run`)"
    if post_state["missing"]:
        state_check = "no KCTL_STATE line seen (state check did not complete)"
    else:
        state_check = "KCTL_STATE seen for " + ", ".join(sorted(post_state["states"]))
    halted = post_state["still_halted"]
    halted_txt = ("still_halted=" + ",".join(halted) + " (core not running: dark-board path)") \
        if halted else "still_halted=none"
    return f"reset markers: KCTL_RESET_ISSUED seen={issued}; {state_check}; {halted_txt}"


def _esp_profile_running_refusal(action: str, unreadable_warnings: "Optional[list]" = None) -> "Optional[str]":
    """Refusal text if the ESP reports a running/paused profile or a live
    autotune run, OR if that state cannot be read (fail closed: an
    unreadable executor is not evidence of idle). allow_running=True
    (exactly True) overrides, at the callers.

    If `unreadable_warnings` is a list, an UNREADABLE state (recovery image,
    bricked, link down) is not a refusal: a WARNING line is appended to the
    list and the check continues; only a CONFIRMED running profile/autotune
    refuses (owner decision 2026-10-10, review B1; debug_program pico)."""
    override = "Stop the run first, or pass allow_running=True (exactly True) to override."
    try:
        status = _srv._profiles.get_exec_status(timeout=2.0)
    except Exception as exc:  # noqa: BLE001
        status = None
        err = str(exc)
    else:
        err = "no answer"
    if status is None and unreadable_warnings is not None:
        unreadable_warnings.append(
            f"WARNING: the ESP profile executor state could not be read ({err}) -- "
            "ESP is in recovery, bricked or the link is down; a firing was NOT ruled out.")
        return None
    if status is None:
        return (f"error: refusing to {action} ESP -- profile executor state could not be read "
                f"({err}), so a firing cannot be ruled out. {override}")
    if status.state not in (0, 3, 4):
        return (
            f"error: refusing to {action} ESP while a profile is {status.state_name} (running/paused/unknown) -- "
            "this would freeze or interrupt relay control mid-firing. " + override
        )
    try:
        at = _srv._autotune.get_status()
    except Exception as exc:  # noqa: BLE001
        if unreadable_warnings is not None:
            unreadable_warnings.append(
                f"WARNING: the ESP autotune state could not be read ({exc}); proceeding.")
            return None
        return (f"error: refusing to {action} ESP -- autotune state could not be read ({exc}). "
                + override)
    if at.state not in (0, 5, 6):
        return (f"error: refusing to {action} ESP while autotune is {at.state_name!r} -- "
                "a halt would freeze the relay control loop. " + override)
    return None


@_core._tool()
def debug_reset(
    peer: str,
    mode: str = "run",
    verify: bool = True,
    verify_window_s: float = reset_probe.DEFAULT_WINDOW_S,
    allow_dark_rereset: bool = False,
    allow_running: bool = False,
) -> str:
    """Resets `peer` ("esp"/"pico"). `mode` is "run" (default, resumes
    execution), "halt" (resets and halts), or "init" (resets and runs any
    OpenOCD target init sequence, then halts).

    For peer="esp" in mode "run", a successful OpenOCD reset is NOT taken as
    proof the board came back: with `verify=True` (default) this polls for up
    to `verify_window_s` seconds for the board answering GET /api/boot_guard
    (and the UART link), reports time-to-answer, boot_count/persisted_count/
    recovery_mode, and returns a loud WARNING if nothing answers or the board
    is in recovery mode. It only reports -- it never resumes or resets.
    Every call appends one JSON line to logs/debug_reset/history.jsonl
    (gitignored, not rotated).

    Cost: an ESP run-mode reset now blocks while it verifies -- typically ~20-25 s (bench-measured 2026-10-01: UART 20.0 s, HTTP 23.2 s)
    on a healthy board, up to the full `verify_window_s` on a silent one.
    Pass verify=False to skip.

    Guard (peer="esp", any mode): REFUSES if the previous ESP reset in
    history.jsonl was a run-mode reset whose probe never got an HTTP answer and
    it was less than reset_probe.LINK_DEAD_HARD_S_DEFAULT (120 s, the firmware
    default of the Pico's configurable link_dead_hard_s) ago, measured from
    the reset itself (the history ts is written after the probe, so the probe's
    elapsed time is added). A JTAG reset sends
    no ANNOUNCE_REBOOT grace, so resetting an ESP that is still dark extends the
    link silence toward S6b (SAFETY_TRIP_LINK_DEAD, mask 0x0040), and clearing
    S6b needs owner authorization. The refusal states how many seconds remain
    in the window. Pass allow_dark_rereset=True (exactly True) to override. A
    missing/corrupt history never blocks. The Pico peer is not gated: resetting
    the Pico does not lengthen ESP link silence (it re-handshakes on boot).

    Guard (peer="esp"): also REFUSES while a profile is running or paused
    (a reset mid-firing drops relay control); pass allow_running=True
    (exactly True) to override."""
    if peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("reset")
        if refusal is not None:
            return refusal
    if peer == debug_probe.PEER_ESP and allow_dark_rereset is not True:
        dark = reset_probe.recent_dark_esp_reset(debug_probe._repo_root())
        if dark is not None:
            return (
                f"error: refusing ESP reset -- the previous ESP reset ({dark.ts}, "
                f"{dark.age_s:.0f}s ago) " + (
                    "left the core not running (state check after reset run), so the board is dark. "
                    if dark.cause == "not_running" else
                    "never got an HTTP answer, so the board may still be dark. ") +
                "A JTAG reset sends no ANNOUNCE_REBOOT grace; stacking resets extends the ESP link "
                f"silence toward S6b (SAFETY_TRIP_LINK_DEAD, mask 0x0040) at "
                f"{reset_probe.LINK_DEAD_HARD_S_DEFAULT:.0f}s (firmware default of link_dead_hard_s, "
                f"about {dark.remaining_s:.0f}s from now if the board stays dark) and clearing S6b needs owner authorization. "
                "Check the board first (boot_guard_get / safety_get_status), wait out the window, "
                "or pass allow_dark_rereset=True to proceed anyway."
                + (
                    " NOTE: UART answered; HTTP did not -- may be a Wi-Fi/host issue, "
                    "not a dark ESP; override if so."
                    if dark.uart_answered else ""
                )
            )
    record: dict = {"peer": peer, "mode": mode, "openocd_ok": None, "probe": None}

    def _append() -> None:
        hist_err = reset_probe.append_history(debug_probe._repo_root(), record)
        if hist_err:
            _srv._session_log.warning("debug_reset: history append failed: %s", hist_err)

    try:
        ok, output = debug_probe.reset(peer, mode)
    except Exception as exc:  # noqa: BLE001 - recorded, then re-raised unchanged
        record["reset_raised"] = f"{type(exc).__name__}: {exc}"
        _append()
        raise
    record["openocd_ok"] = ok
    _log_openocd_result(f"debug_reset(peer={peer}, mode={mode})", ok, output)
    post_state = None
    if peer == debug_probe.PEER_ESP and mode == "run":
        post_state = debug_probe.parse_post_reset(output)
        record["post_reset_states"] = {**post_state["states"], **{
            t: f"{post_state['states'].get(t, '?')}->{st}" for t, st in post_state["final"].items()}}
        record["resumed_targets"] = post_state["resumed"]
        board_dark = (not ok) and post_state["dark"]
        if board_dark:
            # Known not-running after reset run: the board is dark, so the
            # dark-rereset guard must treat this like a probe-dark reset.
            record["still_halted"] = True
    probe_res = None
    probe_error = None
    esp_run = peer == debug_probe.PEER_ESP and mode == "run"
    if ok and esp_run:
        if verify:
            try:
                probe_res = _probe_esp_after_reset(verify_window_s)
                record["probe"] = probe_res.to_json()
            except Exception as exc:  # noqa: BLE001 - the reset result must survive
                probe_error = f"{type(exc).__name__}: {exc}"
                record["probe_error"] = probe_error
        else:
            record["probe_skipped"] = "verify=False"
    if not ok:
        record["openocd_decisive_line"] = _decisive_openocd_line(output)
    _append()
    if not ok and post_state is not None and post_state["dark"]:
        what = (
            "no post-reset state was reported (check did not complete)" if post_state["missing"]
            else "target(s) " + ", ".join(f"{t}={st}" for t, st in sorted(post_state["not_running"].items()))
            + " (not `running`) after `reset run` and a fallback resume"
        )
        return (
            f"error: reset {peer} (run) FAILED -- {what}; the core is NOT confirmed running "
            "(board will likely be dark). Do not stack resets blindly; inspect with debug_read_registers.\n"
            + _reset_marker_line(post_state) + "\n"
            + output.strip()
        )
    if ok:
        msg = f"reset {peer} ({mode}) OK"
        marker_line = _reset_marker_line(post_state)
        if marker_line:
            msg += "\n" + marker_line
        if post_state is not None and post_state["states"]:
            msg += "\npost-reset target states: " + ", ".join(
                f"{t}={st}" for t, st in post_state["states"].items())
            if post_state["resumed"]:
                msg += (f"\nNOTE: fallback resume issued for {', '.join(post_state['resumed'])} "
                        "(core was still halted after `reset run`)")
        if probe_res is not None:
            msg += "\n" + reset_probe.format_report(peer, mode, probe_res)
        elif probe_error is not None:
            msg += (
                f"\nWARNING: the post-reset reachability probe itself raised ({probe_error}); "
                "the board's state after the reset is UNVERIFIED."
            )
        return msg
    failed = _openocd_error_message(f"reset failed for {peer}", output)
    marker_line = _reset_marker_line(post_state)
    return f"{failed}\n{marker_line}" if marker_line else failed


def _probe_esp_after_reset(window_s: float) -> "reset_probe.ProbeResult":
    from .mcp_server_flash import _resolve_verify_hosts  # local: avoids a circular import

    def uart_fw(timeout: float = reset_probe.UART_ATTEMPT_TIMEOUT_S):
        return _srv._info.get_fw_version(timeout=timeout)

    return reset_probe.probe_after_reset(
        hosts_fn=lambda: _resolve_verify_hosts(None),
        get_boot_guard=ota_http.get_boot_guard_status,
        get_uart_fw=uart_fw,
        window_s=window_s,
    )


@_core._tool()
def debug_halt(peer: str, allow_running: bool = False) -> str:
    """Halts `peer`'s core.

    Guard: for peer="esp", refuses if a fire profile is currently running or
    paused (halting the ESP mid-profile freezes relay control and stops its
    telemetry to the Pico, which would correctly read it as a dead main
    controller and trip -- see debug_probe.py's module docstring). If the ESP
    doesn't answer the status query at all, or the executor/autotune state is
    unreadable or unknown, the halt is REFUSED (fail closed). Pass allow_running=True (exactly True) to override.
    """
    if peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("halt")
        if refusal is not None:
            return refusal
    ok, output = debug_probe.halt(peer)
    _log_openocd_result(f"debug_halt(peer={peer})", ok, output)
    if ok:
        return f"halted {peer}"
    return _openocd_error_message(f"halt failed for {peer}", output)


@_core._tool()
def debug_resume(peer: str) -> str:
    """Resumes `peer`'s core if halted. Reports per target (state before/after);
    a target that is already running is reported "no resume needed", not an error."""
    ok, output = debug_probe.resume(peer)
    _log_openocd_result(f"debug_resume(peer={peer})", ok, output)
    if ok:
        return f"resume {peer}:\n{output}"
    return _openocd_error_message(f"resume failed for {peer}", output)


@_core._tool()
def debug_step(peer: str, allow_running: bool = False) -> str:
    """Single-steps `peer`'s core one instruction. If it was running, this
    halts it first (it does not resume running after the step -- it stays
    halted at the next instruction). For peer="esp" refuses while a profile or
    autotune runs (or their state is unreadable) unless allow_running=True."""
    if peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("step")
        if refusal is not None:
            return refusal
    ok, output = debug_probe.step(peer)
    _log_openocd_result(f"debug_step(peer={peer})", ok, output)
    if ok:
        return f"stepped {peer}:\n{output.strip()}"
    return _openocd_error_message(f"step failed for {peer}", output)


@_core._tool()
def debug_read_memory(peer: str, address: int, count: int = 1, width: int = 32,
                      leave_halted: bool = False,
                      target: str | None = None,
                      allow_running: bool = False) -> str:
    """Reads `count` `width`-bit (8/16/32) words from `peer`'s memory starting
    at `address`. Read-only, no guard needed. Halts the core if it wasn't
    already (OpenOCD requires this for a memory read), then resumes it unless
    `leave_halted` is set -- a read that exits without resuming leaves the
    board halted, which reads as "the firmware froze" to everything except a
    debugger. Set `leave_halted` only when several reads must observe the same
    frozen state.

    `target` picks one core by OpenOCD target name ("rp2040.core0" /
    "rp2040.core1"), as `debug_read_registers` does. Pass it whenever the
    address is in the private peripheral bus (0xE0000000-0xE00FFFFF): the NVIC
    and parts of the SCB are banked per core on the RP2040, so NVIC_ISER
    (0xE000E100) or VTOR (0xE000ED08) read without naming a core describes
    core 0 only, whichever core you meant."""
    if leave_halted is True and peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("leave halted")
        if refusal is not None:
            return refusal
    ok, output = debug_probe.read_memory(peer, address, count, width,
                                         leave_halted=leave_halted,
                                         target=target)
    _log_openocd_result(f"debug_read_memory(peer={peer}, address=0x{address:x})", ok, output)
    if ok:
        return output.strip()
    return _openocd_error_message(f"read_memory failed for {peer}", output)


@_core._tool()
def debug_read_symbol(peer: str, symbol: str, count: Optional[int] = None, width: int = 32,
                      elf_path: Optional[str] = None, leave_halted: bool = False,
                      allow_running: bool = False) -> str:
    """Reads a named symbol from `peer`'s memory ("esp" or "pico"). Same as
    debug_read_memory() but resolves the address from the peer's build ELF,
    so a counter or register image can be read by name without looking up an
    address by hand.

    `count` defaults to the symbol's whole recorded size at the requested
    `width` (a 16-byte array reads as 16 bytes with width=8), or one word if
    the ELF records no size.

    Two caveats worth knowing before trusting the numbers:
      - A symbol defined at more than one address (file-static objects of the
        same name in different translation units) is
        refused rather than resolved arbitrarily.
      - This returns raw bytes and knows nothing about struct layout. Get a
        member's offset from DWARF (`objdump --dwarf=info`); assuming offset 0
        is how a correct register image was misread as garbage on 2026-08-25.
    """
    if leave_halted is True and peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("leave halted")
        if refusal is not None:
            return refusal
    try:
        ok, output = debug_probe.read_symbol(peer, symbol, count=count, width=width,
                                             elf_path=elf_path, leave_halted=leave_halted)
    except (ValueError, FileNotFoundError, RuntimeError) as exc:
        return f"error: {exc}"
    _log_openocd_result(f"debug_read_symbol(peer={peer}, symbol={symbol})", ok, output)
    if ok:
        return output.strip()
    return _openocd_error_message(f"read_symbol failed for {peer}", output)


@_core._tool()
def debug_list_symbols(peer: str, pattern: str, elf_path: Optional[str] = None, limit: int = 40) -> str:
    """Lists symbols in `peer`'s build ELF whose name contains `pattern`
    (case-insensitive substring, not a regex), with address and recorded size.
    Use it to find what debug_read_symbol() can read -- e.g. pattern="g_dbg"
    for temporary bench counters.

    A symbol shown as "AMBIGUOUS" is defined at several addresses in this ELF
    and debug_read_symbol() will refuse it by name."""
    try:
        table = debug_probe.symbol_table(peer, elf_path)
    except (ValueError, FileNotFoundError, RuntimeError) as exc:
        return f"error: {exc}"
    needle = pattern.lower()
    hits = sorted(name for name in table if needle in name.lower())
    if not hits:
        return f"no symbol name contains {pattern!r}"
    lines = []
    for name in hits[:limit]:
        address, size = table[name]
        if address < 0:
            lines.append(f"{name}: AMBIGUOUS (defined at multiple addresses)")
        else:
            lines.append(f"0x{address:08x}  size {size:5d}  {name}")
    if len(hits) > limit:
        lines.append(f"... and {len(hits) - limit} more (raise limit or narrow the pattern)")
    return "\n".join(lines)


def _write_readback_note(peer: str, address: int, value: int, width: int) -> str:
    try:
        rok, rout = debug_probe.read_memory(peer, address, 1, width)
    except Exception as exc:  # noqa: BLE001 - the write already happened
        return f"\nWARNING: read-back raised ({type(exc).__name__}: {exc}); write UNVERIFIED."
    m = re.search(r"MEMRD 0x[0-9a-fA-F]+ 0x([0-9a-fA-F]+)", rout or "") if rok else None
    if m is None:
        return "\nWARNING: read-back failed or unparseable; write UNVERIFIED."
    got = int(m.group(1), 16)
    if got != value & ((1 << width) - 1):
        return (f"\nFAILED: read-back 0x{got:x} != written 0x{value:x} -- the write did not "
                "take, or the location is a volatile register that legitimately differs.")
    return f"\nread-back OK (0x{got:x})"


@_core._tool()
def debug_write_memory(
    peer: str, address: int, value: int, width: int = 32, confirm: bool = False, allow_running: bool = False
) -> str:
    """Writes one `width`-bit (8/16/32) `value` at `address` in `peer`'s
    memory. Live RAM/flash-mapped memory write on a running board -- refused
    unless `confirm=True` is passed explicitly.

    Additive guard for peer="pico": even with confirm=True, this reads
    SaftyFW's relay_owner.c ARMED/GRACE/TRIPPED state directly over SWD first
    (debug_probe.pico_armed_state() -- UART-link-independent, see
    debug_probe.py's module docstring) and refuses the write if that state
    reads ARMED, OR if it could not be confidently determined at all (fail
    closed -- an unreadable state is never treated as "not armed"). This is
    additive to, never a replacement for, the confirm=True gate below.

    For peer="esp" it also refuses while a profile is running or paused
    (allow_running=True, exactly True, overrides). After a successful write
    the word is read back; a mismatch is reported as FAILED (a volatile
    register legitimately differs but is still not the written value), an unreadable read-back as unverified."""
    if confirm is not True:
        return "error: memory write refused without confirm=True -- this writes live RAM/flash-mapped memory on a running board"
    if peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("write memory on")
        if refusal is not None:
            return refusal
    if peer == debug_probe.PEER_PICO:
        armed, detail = debug_probe.pico_armed_state()
        if armed is not False:
            _srv._session_log.warning(
                "debug_write_memory: REFUSED peer=pico address=0x%x armed_state=%s detail=%s",
                address, armed, detail,
            )
            if armed is None:
                return f"error: write refused -- could not confidently determine Pico ARMED state ({detail})"
            return f"error: write refused -- Pico is ARMED ({detail})"
    ok, output = debug_probe.write_memory(peer, address, value, width)
    if ok:
        _srv._session_log.warning(
            "debug_write_memory: peer=%s address=0x%x value=0x%x width=%d ok=True", peer, address, value, width
        )
        msg = f"wrote 0x{value:x} ({width}-bit) to {peer} 0x{address:x}"
        note = _write_readback_note(peer, address, value, width)
        if "FAILED:" in note:
            return "FAILED - " + msg + note
        if "UNVERIFIED" in note:
            return "UNVERIFIED - " + msg + note
        return msg + note
    _log_openocd_result(
        f"debug_write_memory(peer={peer}, address=0x{address:x}, value=0x{value:x}, width={width})", ok, output
    )
    return _openocd_error_message(f"write_memory failed for {peer}", output)


@_core._tool()
def debug_read_registers(peer: str, target: str | None = None,
                         leave_halted: bool = False, allow_running: bool = False) -> str:
    """Reads the core registers for `peer`. Needs the core halted to read
    registers, so this halts it as a side effect if it was running.

    `target` picks one core by OpenOCD target name on a multi-core chip --
    "rp2040.core0" / "rp2040.core1" for the Pico. Omit it to read whichever
    core the config makes current (core 0 on the Pico).

    Resumes the core afterwards unless `leave_halted` is set, for the same
    reason `debug_read_memory` does.
    """
    if leave_halted is True and peer == debug_probe.PEER_ESP and allow_running is not True:
        refusal = _esp_profile_running_refusal("leave halted")
        if refusal is not None:
            return refusal
    ok, output = debug_probe.read_registers(peer, target=target,
                                            leave_halted=leave_halted)
    _log_openocd_result(f"debug_read_registers(peer={peer}, target={target})", ok, output)
    if ok:
        return output.strip()
    return _openocd_error_message(f"read_registers failed for {peer}", output)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
