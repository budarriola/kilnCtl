"""Standalone recovery image -- MCP surface for firmware/KilnFW_recovery/'s
HTTP routes (recovery_http.c; docs/RECOVERY_IMAGE_PLAN.md). Only meaningful
against a board that has actually fallen back to the recovery image (boot_guard
recovery mode); against a normally running board every tool here fails its
first status read (404) and does nothing.

Read-only:   recovery_status                     (GET /api/recovery/status + /pico/status)
Mutating:    recovery_exit, recovery_wifi_reset, recovery_boot_guard_reset,
             recovery_pico_upload
Every mutating tool: refuses unless ``confirm is True`` EXACTLY (before any
network access), needs the AP password in KILNCTL_AP_PASSWORD (read here,
never a parameter, never printed -- only a [bool] is ever reported), reads
status BEFORE acting and AFTER, and fails loud when the read-back disagrees
with what the board's own reply claimed. Signing is
recovery_ota_auth_client.py (query-bound MAC, see its derive_mac()).

Never run against real hardware from tests: tests/test_mcp_server_recovery.py
fakes both HTTP layers.
"""
from __future__ import annotations

import json
import os
import time
import zlib
from typing import Optional

from . import recovery_http_client as rhc
from . import recovery_ota_auth_client as roac

from . import mcp_server as _srv

AP_PASSWORD_ENV = "KILNCTL_AP_PASSWORD"

#: Image sanity cap for the Pico upload (a SaftyFW slot image is well under
#: this; the board enforces its own, tighter limits and CRC/vector checks).
MAX_PICO_IMAGE_BYTES = 4 * 1024 * 1024

#: Indirections so tests never sleep.
_sleep = time.sleep
_monotonic = time.monotonic


def _resolve_host(host: Optional[str]) -> str:
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as the other tools
    return _ota_resolve_host(host)


def _ap_password() -> Optional[str]:
    pw = os.environ.get(AP_PASSWORD_ENV, "")
    return pw or None


def _refuse_unconfirmed(what: str) -> str:
    return f"REFUSED: {what} is destructive/disruptive -- pass confirm=True (exactly True) to proceed"


def _refuse_no_password() -> str:
    return (f"REFUSED: {AP_PASSWORD_ENV} set=False -- the recovery image's HMAC key is the AP "
            f"password, read only from that environment variable (never a parameter)")


def _fmt_status(st: dict) -> str:
    keys = ("running", "app_present", "app_size", "app_desc_present", "app_valid", "record_present",
            "boot_count", "relay_fault", "relays_verified_off", "nvs_unavailable", "nvs_failed_mask",
            "heap_internal_min_free")
    return ", ".join(f"{k}={st[k]}" for k in keys if k in st)


def _fmt_pico(p: dict) -> str:
    keys = ("phase", "busy", "psram", "bytes_sent", "total_bytes", "gap_count", "pico_mode",
            "target_slot", "target_source", "power_cycle", "refusal", "error", "message")
    return ", ".join(f"{k}={p[k]!r}" if isinstance(p[k], str) else f"{k}={p[k]}"
                     for k in keys if k in p and p[k] != "")


def _preflight(host: str) -> "tuple[Optional[dict], Optional[dict], Optional[str]]":
    """Reads both status routes. Returns (status, pico, error_string)."""
    try:
        st = rhc.get_status(host)
    except rhc.RecoveryHttpError as exc:
        if exc.status == 404:
            return None, None, (f"GET /api/recovery/status answered 404 (host={host}) -- this is not the "
                                f"recovery image (the normal application is running?); nothing done")
        return None, None, f"could not read GET /api/recovery/status (host={host}): {exc}"
    if st.get("running") != "recovery":
        return None, None, (f"status says running={st.get('running')!r}, not 'recovery' (host={host}); "
                            f"nothing done")
    try:
        pico = rhc.get_pico_status(host)
    except rhc.RecoveryHttpError as exc:
        return None, None, f"could not read GET /api/recovery/pico/status (host={host}): {exc}"
    return st, pico, None


def _post_error(path: str, exc: "roac.RecoveryOtaAuthError") -> str:
    # exc text never contains the password; only status/detail from the board.
    return f"FAILED: POST {path} -- {exc}"


def _poll_restart(host: str, wait_s: float, interval_s: float = 1.0) -> "tuple[str, str]":
    """After a POST that makes the board restart. Returns (verdict, detail):
    'app'  -- recovery routes answer 404: the normal application is up
    'recovery_again' -- the board dropped off and came back AS the recovery image
    'never_restarted' -- it never dropped and still answers as recovery
    'silent' -- it dropped and has not answered by the deadline."""
    deadline = _monotonic() + wait_s
    dropped = False
    while True:
        _sleep(interval_s)
        try:
            st = rhc.get_status(host, timeout=3.0)
        except rhc.RecoveryHttpError as exc:
            if exc.status == 404:
                return "app", "GET /api/recovery/status -> 404"
            if exc.status is None:
                dropped = True
        else:
            if st.get("running") == "recovery" and dropped:
                return "recovery_again", _fmt_status(st)
        if _monotonic() >= deadline:
            return ("silent" if dropped else "never_restarted"), ""


@_srv._tool()
def recovery_status(host: Optional[str] = None) -> str:
    """READ-ONLY: the standalone recovery image's own status --
    GET /api/recovery/status (running partition, app image presence/size/
    full-image validity, boot_guard record, relay fault, NVS availability,
    heap floor) plus GET /api/recovery/pico/status (Pico relay phase/busy/
    bytes/outcome). Both routes are unauthenticated on the board. No password
    is read. Against a board running the normal application this reports a
    404, which is itself the evidence that the recovery image is not running.

    Note: while the Pico relay is busy, reading its status keeps the relay's
    'operator still watching' timer alive (the board's own design).
    """
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    notes = []
    if st.get("nvs_unavailable"):
        notes.append("NVS UNAVAILABLE (recovery never erases it)")
    if st.get("relay_fault") or st.get("relays_verified_off") is False:
        notes.append("RELAY FAULT / relays not verified off")
    if pico.get("phase") == "outcome_unknown":
        notes.append("last Pico transfer ended OUTCOME UNKNOWN -- power-cycle and check the Pico version")
    return (f"recovery image (host={resolved}): {_fmt_status(st)}\n"
            f"pico relay: {_fmt_pico(pico)}" + (("\nWARNING: " + "; ".join(notes)) if notes else ""))


@_srv._tool()
def recovery_exit(confirm: bool = False, host: Optional[str] = None, wait_s: float = 60.0) -> str:
    """Leave the recovery image and boot the application (POST
    /api/recovery/exit, AP-password X-Ota-Mac context "recovery-exit"). The
    board verifies `app` is a valid image (409 otherwise), clears boot_guard,
    selects `app` and restarts.

    REFUSES unless ``confirm is True`` exactly; needs KILNCTL_AP_PASSWORD
    (reports presence as a bool only). Reads recovery status first and refuses
    if the Pico relay is busy or ``app_valid`` is not true. After the POST it
    polls until the recovery routes answer 404 (the application is up) and
    FAILS LOUDLY if the board instead never restarted or came back as the
    recovery image; a board that restarted but has not answered by `wait_s`
    is reported UNVERIFIED, never ok.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_exit (reboots the board into the application)")
    pw = _ap_password()
    if pw is None:
        return _refuse_no_password()
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("busy"):
        return f"REFUSED: the Pico relay is busy ({_fmt_pico(pico)}); the board would 409 and a restart would kill it"
    if st.get("app_valid") is not True:
        return f"REFUSED: app_valid is {st.get('app_valid')!r} -- no valid application image to boot ({_fmt_status(st)})"
    try:
        reply = roac.recovery_exit(resolved, pw)
    except roac.RecoveryOtaAuthError as exc:
        return _post_error("/api/recovery/exit", exc)
    verdict, detail = _poll_restart(resolved, wait_s)
    if verdict == "app":
        return f"ok - board accepted exit ({reply['text']!r}) and the application is answering ({detail}) (host={resolved})"
    if verdict == "recovery_again":
        return (f"FAILED: board accepted exit ({reply['text']!r}) but came back as the RECOVERY image "
                f"({detail}) -- the application did not boot (host={resolved})")
    if verdict == "never_restarted":
        return (f"FAILED: board accepted exit ({reply['text']!r}) but never restarted within {wait_s:g}s and "
                f"still answers as the recovery image (host={resolved})")
    return (f"UNVERIFIED: board accepted exit ({reply['text']!r}) and went silent, but nothing answered by "
            f"{wait_s:g}s -- check by hand, do not assume the application booted (host={resolved})")


@_srv._tool()
def recovery_wifi_reset(confirm: bool = False, host: Optional[str] = None, wait_s: float = 60.0) -> str:
    """Forget the HOME Wi-Fi credentials stored for the recovery image and
    restart (POST /api/recovery/wifi_reset, context "wifi-reset"). The AP
    name/password are kept (the password is this image's HMAC key).

    REFUSES unless ``confirm is True`` exactly; needs KILNCTL_AP_PASSWORD
    (bool only). Reads status first, refuses while the Pico relay is busy.
    Read-back: the board must drop off and answer again as the recovery image
    (a restart actually happened). The status route does not expose Wi-Fi
    credential state, so the clear itself cannot be read back -- the result
    says so. After the reset the board may come up only on its AP (192.168.4.1),
    so an unanswered host is reported UNVERIFIED, never ok.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_wifi_reset (erases the stored home Wi-Fi credentials and restarts)")
    pw = _ap_password()
    if pw is None:
        return _refuse_no_password()
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("busy"):
        return f"REFUSED: the Pico relay is busy ({_fmt_pico(pico)}); a restart would kill it"
    try:
        reply = roac.recovery_wifi_reset(resolved, pw)
    except roac.RecoveryOtaAuthError as exc:
        return _post_error("/api/recovery/wifi_reset", exc)
    verdict, detail = _poll_restart(resolved, wait_s)
    if verdict == "recovery_again":
        return (f"ok - board replied {reply['text']!r} and restarted back into the recovery image ({detail}) "
                f"(host={resolved}). The credential clear itself is not readable from the status route; "
                f"it is the board's own reply only")
    if verdict == "app":
        return (f"FAILED: board replied {reply['text']!r} but the host now answers as the normal application "
                f"(recovery routes 404) -- unexpected for wifi_reset (host={resolved})")
    if verdict == "never_restarted":
        return (f"FAILED: board replied {reply['text']!r} but never restarted within {wait_s:g}s "
                f"(host={resolved})")
    return (f"UNVERIFIED: board replied {reply['text']!r} and went silent; nothing answered at {resolved} by "
            f"{wait_s:g}s (without home Wi-Fi it may only be reachable on its AP, 192.168.4.1)")


@_srv._tool()
def recovery_boot_guard_reset(confirm: bool = False, host: Optional[str] = None) -> str:
    """Clear the boot_guard counter from the recovery image (POST
    /api/ota/esp/boot_guard_reset on the RECOVERY image, context
    "boot-guard-reset"; not the main app's route of the same path). The board
    erases the record in both locations and reads it back itself.

    REFUSES unless ``confirm is True`` exactly; needs KILNCTL_AP_PASSWORD
    (bool only). Reads recovery status first (``record_present``/
    ``boot_count``) and does nothing if no record exists. Re-reads status
    afterward and FAILS LOUDLY if ``record_present`` is still not false -- a
    200 reply is not trusted alone.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_boot_guard_reset")
    pw = _ap_password()
    if pw is None:
        return _refuse_no_password()
    resolved = _resolve_host(host)
    before, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if before.get("record_present") is False:
        return f"nothing to clear -- boot_guard record_present=False before ({_fmt_status(before)}) (host={resolved})"
    try:
        reply = roac.recovery_boot_guard_reset(resolved, pw)
    except roac.RecoveryOtaAuthError as exc:
        return _post_error("/api/ota/esp/boot_guard_reset", exc)
    try:
        after = rhc.get_status(resolved)
    except rhc.RecoveryHttpError as exc:
        return (f"UNVERIFIED: board replied {reply['text']!r} but the confirming status read failed "
                f"(host={resolved}): {exc}")
    if after.get("record_present") is False:
        return (f"ok - boot_guard cleared and confirmed by read-back: before boot_count="
                f"{before.get('boot_count', 'n/a')}, after record_present=False (host={resolved})")
    return (f"FAILED: board replied {reply['text']!r} but status still shows record_present="
            f"{after.get('record_present')!r} boot_count={after.get('boot_count', 'n/a')} -- "
            f"not cleared (host={resolved})")


@_srv._tool()
def recovery_pico_upload(image_path: str, confirm: bool = False, slot: Optional[str] = None,
                         host: Optional[str] = None, wait_s: float = 600.0) -> str:
    """Flash a SaftyFW slot image (.bin) onto the RP2040 through the recovery
    image's UART relay (POST /api/recovery/pico/upload?crc=<hex>[&slot=A|B],
    context "pico-upload", query bound into the MAC -- the CRC32 is computed
    here from the file; `slot` is the operator's target-slot assertion, None
    for auto). The board validates size/vectors/CRC (422 on a bad image) and
    answers 202 when the relay starts; the OUTCOME is only in the status poll.

    REFUSES unless ``confirm is True`` exactly, the file is an existing
    absolute path, and KILNCTL_AP_PASSWORD is set (bool only). Reads both
    status routes first and REFUSES unless the relay is idle (``busy`` false,
    psram true). After the 202 it polls /api/recovery/pico/status until a
    terminal phase or `wait_s`, and reports honestly:
      done            -> "ok" only if bytes_sent == total_bytes (else FAILED)
      failed          -> FAILED with the board's refusal/error/message
      aborted         -> ABORTED
      outcome_unknown -> OUTCOME UNKNOWN, explicitly NOT success: END was sent
                         and the result lost; power-cycle and check the Pico
                         version, do not retry blindly
      still running at `wait_s` / contact lost -> UNKNOWN (never ok)
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_pico_upload (reflashes the safety processor)")
    if slot not in (None, "A", "B"):
        return f"REFUSED: slot must be 'A', 'B' or None (auto), got {slot!r}"
    if not isinstance(image_path, str) or not os.path.isabs(image_path):
        return "REFUSED: image_path must be an absolute path"
    try:
        size = os.path.getsize(image_path)
        if size <= 0 or size > MAX_PICO_IMAGE_BYTES:
            return f"REFUSED: image is {size} bytes (want 1..{MAX_PICO_IMAGE_BYTES})"
        with open(image_path, "rb") as fh:
            image = fh.read()
    except OSError as exc:
        return f"REFUSED: cannot read image_path: {exc}"
    pw = _ap_password()
    if pw is None:
        return _refuse_no_password()
    crc = zlib.crc32(image) & 0xFFFFFFFF
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("psram") is not True:
        return f"REFUSED: the board reports no PSRAM for the relay ({_fmt_pico(pico)})"
    if pico.get("busy") is not False:
        return f"REFUSED: the Pico relay is not idle ({_fmt_pico(pico)}); abort or wait first"
    try:
        reply = roac.recovery_pico_upload(resolved, image, crc, pw, slot=slot)
    except roac.RecoveryOtaAuthError as exc:
        return _post_error("/api/recovery/pico/upload", exc)
    try:
        started = reply["status"] == 202 and json.loads(reply["text"]).get("started") is True
    except ValueError:
        started = False
    if not started:
        return f"FAILED: upload POST did not report a started relay (HTTP {reply['status']}: {reply['text'][:120]!r})"

    lost = 0
    last: dict = {}
    deadline = _monotonic() + wait_s
    while True:
        _sleep(1.0)
        try:
            last = rhc.get_pico_status(resolved)
            lost = 0
        except rhc.RecoveryHttpError as exc:
            lost += 1
            if lost >= 5:
                return (f"UNKNOWN: lost contact with the board while the relay was running ({exc}); "
                        f"last status: {_fmt_pico(last) or 'none'} -- NOT success, re-check by hand")
            continue
        phase = last.get("phase")
        if phase in rhc.PICO_TERMINAL_PHASES:
            break
        if _monotonic() >= deadline:
            return (f"UNKNOWN: relay still running after {wait_s:g}s ({_fmt_pico(last)}) -- NOT success; "
                    f"poll recovery_status")
    prefix = f"crc32={crc:08x} slot={slot or 'auto'} (host={resolved})"
    if phase == "done":
        if last.get("total_bytes") and last.get("bytes_sent") == last.get("total_bytes") == len(image):
            return f"ok - relay reports done, {last['bytes_sent']}/{last['total_bytes']} bytes sent; {_fmt_pico(last)}; {prefix}"
        return (f"FAILED: relay reports done but bytes_sent/total_bytes disagree with the {len(image)}-byte "
                f"image ({_fmt_pico(last)}); {prefix}")
    if phase == "outcome_unknown":
        return (f"OUTCOME UNKNOWN - NOT success: the relay stopped after END was sent and the result was lost. "
                f"Power-cycle and check the Pico version; do NOT retry blindly. {_fmt_pico(last)}; {prefix}")
    if phase == "aborted":
        return f"ABORTED: {_fmt_pico(last)}; {prefix}"
    return f"FAILED: relay phase=failed: {_fmt_pico(last)}; {prefix}"
