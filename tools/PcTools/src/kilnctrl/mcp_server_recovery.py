"""Standalone recovery image -- MCP surface for firmware/KilnFW_recovery/'s
HTTP routes (recovery_http.c; docs/RECOVERY_IMAGE_PLAN.md). Only meaningful
against a board that has actually fallen back to the recovery image (boot_guard
recovery mode); against a normally running board every tool here fails its
first status read (404) and does nothing.

Read-only:   recovery_status                     (GET /api/recovery/status + /pico/status)
Mutating:    recovery_exit, recovery_wifi_reset, recovery_boot_guard_reset,
             recovery_pico_upload, recovery_pico_abort, recovery_sw_reset,
             recovery_push_esp_image
Every mutating tool: refuses unless ``confirm is True`` EXACTLY (before any
network access), reads status BEFORE acting and AFTER, and fails loud when the read-back
disagrees with what the board's own reply claimed.

The recovery image is UNAUTHENTICATED (owner decision 2026-10-02): no password,
no key, no signing. Its only access control is physical -- a WPA2 SoftAP with a
random per-boot passphrase shown on the board's LCD. The PC must already be
joined to that AP (env KILNCTL_RECOVERY_AP_PASSPHRASE is for whatever joins it;
no tool here reads or needs it). POSTs are in recovery_post_client.py.

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
from . import recovery_post_client as rpc

from . import mcp_server as _srv

#: recovery_pico_proto.h RPP_SLOT_SIZE (832 KB): recovery_pico_reserve()
#: answers 413 above this.
MAX_PICO_IMAGE_BYTES = 0x000D0000

#: recovery_push_esp_image() socket timeout: the whole image is streamed in one
#: POST and the board flash-writes while it receives.
ESP_PUSH_TIMEOUT_S = 180.0

#: ESP image header magic byte (esp_image_header_t.magic).
ESP_IMAGE_MAGIC = 0xE9

#: Re-reads of /pico/status when "done" is first seen with bytes_sent not yet
#: equal to the image length: recovery_pico.c publish_done() sets the phase
#: and bytes_sent under two separate lock holds, so one poll can land between.
DONE_REREADS = 3

#: Indirections so tests never sleep.
_sleep = time.sleep
_monotonic = time.monotonic


def _resolve_host(host: Optional[str]) -> str:
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as the other tools
    return _ota_resolve_host(host)


def _refuse_unconfirmed(what: str) -> str:
    return f"REFUSED: {what} is destructive/disruptive -- pass confirm=True (exactly True) to proceed"


def _fmt_status(st: dict) -> str:
    keys = ("running", "app_present", "app_size", "app_desc_present", "app_valid", "record_present",
            "boot_count", "relay_fault", "relays_verified_off", "nvs_unavailable", "nvs_failed_mask",
            "heap_internal_min_free")
    return ", ".join(f"{k}={st[k]}" for k in keys if k in st)


#: GET /api/recovery/status keys added by the 2026-10 diagnostics change
#: (recovery_http.c recovery_status_get), grouped one rendered line per group.
#: A key the board did not send is rendered as NOT_REPORTED, never a value.
_DIAG_GROUPS = (
    ("boot/auth", ("auth_mode", "uptime_s", "reset_reason",
                   "reset_reason_name", "app_ota_state", "coredump_present", "otadata_blank")),
    ("wifi", ("wifi_up", "ap_start_count", "ap_stop_count", "ap_stations", "ap_connect_total",
              "wifi_last_event", "wifi_last_event_age_s")),
    ("relay hold", ("relay_hold_task", "relay_hold_fault", "relay_hold_fault_s",
                    "relay_hold_last_ok_s", "relay_hold_mismatches", "relay_hold_reassert_fails",
                    "relay_hold_stack_free")),
)
NOT_REPORTED = "not reported (older recovery image)"


def _diag_value(st: dict, key: str) -> str:
    if key not in st:
        return NOT_REPORTED
    v = st[key]
    if v is None:
        return "null (board could not read it)"  # JSON null: present but unreadable on the board
    return repr(v) if isinstance(v, str) else str(v)


def _fmt_diag(st: dict) -> str:
    """One line per group of the new diagnostic keys; every key is listed."""
    return "\n".join(f"{name}: " + ", ".join(f"{k}={_diag_value(st, k)}" for k in keys)
                     for name, keys in _DIAG_GROUPS)


def _diag_warnings(st: dict) -> "list[str]":
    """Loud WARNING lines for the diagnostic keys. Only a PRESENT key can warn
    (an absent key is unknown, not healthy and not faulty)."""
    w = []
    ap_stop = st.get("ap_stop_count")
    if isinstance(ap_stop, int) and not isinstance(ap_stop, bool) and ap_stop > 0:
        w.append(f"WARNING: ap_stop_count={ap_stop} -- the soft-AP has been stopped since boot "
                 f"(wifi_last_event={st.get('wifi_last_event')!r}, "
                 f"wifi_last_event_age_s={st.get('wifi_last_event_age_s')})")
    if st.get("relay_hold_fault") is True:
        w.append(f"WARNING: relay_hold_fault=true -- the relay-hold task saw a fault "
                 f"(relay_hold_fault_s={st.get('relay_hold_fault_s')}, "
                 f"relay_hold_mismatches={st.get('relay_hold_mismatches')}, "
                 f"relay_hold_reassert_fails={st.get('relay_hold_reassert_fails')})")
    if st.get("relay_hold_task") is False:
        w.append("WARNING: relay_hold_task=false -- the relay-hold task is NOT running; "
                 "nothing is re-asserting the relays off")
    if st.get("otadata_blank") is True:
        w.append("WARNING: otadata_blank=true -- otadata is blank, so the bootloader boots the "
                 "factory (recovery) image, not `app`")
    if st.get("coredump_present") is True:
        w.append("WARNING: coredump_present=true -- the coredump partition holds a crash dump")
    return w


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


def _post_error(path: str, exc: "rpc.RecoveryPostError") -> str:
    # only status/detail from the board.
    return f"FAILED: POST {path} -- {exc}"


#: recovery_pico.c should_stop(): the relay's own abort when the browser stops
#: polling. finish_stopped() ends in phase "aborted" with this error text, which
#: is indistinguishable by phase from an operator abort.
RELAY_SELF_ABORT_TEXT = "browser stopped polling"

#: recovery_http.c ota_esp_post(): reply is "ok, rebooting into new application
#: image; boot_guard <cleared and verified | boot_guard clear failed (...)>".
BOOT_GUARD_CLEARED_TEXT = "boot_guard cleared and verified"

#: HTTP statuses the board can ONLY answer before the first esp_ota_write()
#: touches `app`: Pico busy (409),
#: recovery_upload_stream()'s length gate / first-chunk RIC_OVERSIZE (413), and
#: its upload-buffer allocation failure (503). 400/422/500 are each emitted both
#: before and after the erase starts ("connection lost mid-image", "image failed
#: verification", "flash write failed"), so they still carry the erase warning.
_PRE_ERASE_STATUSES = (409, 413, 503)


def _boot_guard_outcome(text: str) -> str:
    """The boot_guard part of the board's push reply, or a note when absent."""
    idx = text.find("boot_guard")
    return text[idx:] if idx >= 0 else "reply does not mention boot_guard"


def _app_may_be_erased_note(host: str) -> str:
    """Appended to a push FAILED message: `app` may already be partly erased.
    Re-reads status so the report carries app_valid."""
    note = ("\nWARNING: the board may already have erased part of the `app` partition before this failure; "
            "do NOT trust a reboot until recovery_status shows app_valid=True")
    try:
        st = rhc.get_status(host)
    except rhc.RecoveryHttpError as exc:
        return note + f" (could not re-read status: {exc})"
    return note + f" (re-read just now: app_valid={st.get('app_valid')!r}, app_present={st.get('app_present')!r})"


def _reply_lost(exc: "rpc.RecoveryPostError") -> bool:
    """True when the signed POST was sent but no HTTP status came back
    (timeout, reset): the board may have acted, so this is never a plain
    FAILED -- the caller must go on to observe the board."""
    return exc.status is None and getattr(exc, "stage", "") == "post"


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
    bytes/outcome). Both routes are unauthenticated on the board. Against a board running the normal application this reports a
    404, which is itself the evidence that the recovery image is not running.

    Also renders the diagnostic keys (auth mode, uptime/reset reason,
    coredump/otadata, Wi-Fi AP counters, relay-hold task) one group per line;
    a key an older recovery image does not send reads "not reported (older
    recovery image)", never a made-up value. WARNING lines flag
    ap_stop_count>0, relay_hold_fault, a relay_hold_task not running,
    otadata_blank and coredump_present.

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
    out = (f"recovery image (host={resolved}): {_fmt_status(st)}\n"
           f"pico relay: {_fmt_pico(pico)}" + (("\nWARNING: " + "; ".join(notes)) if notes else ""))
    out += "\n" + _fmt_diag(st)
    for line in _diag_warnings(st):
        out += "\n" + line
    return out


@_srv._tool()
def recovery_exit(confirm: bool = False, host: Optional[str] = None, wait_s: float = 60.0) -> str:
    """Leave the recovery image and boot the application (POST
    /api/recovery/exit). The
    board verifies `app` is a valid image (409 otherwise), clears boot_guard,
    selects `app` and restarts.

    REFUSES unless ``confirm is True`` exactly. Reads recovery status first and refuses
    if the Pico relay is busy or ``app_valid`` is not true. After the POST it
    polls until the recovery routes answer 404 (the application is up) and
    FAILS LOUDLY if the board instead never restarted or came back as the
    recovery image; a board that restarted but has not answered by `wait_s`
    is reported UNVERIFIED, never ok.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_exit (reboots the board into the application)")
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("busy"):
        return f"REFUSED: the Pico relay is busy ({_fmt_pico(pico)}); the board would 409 and a restart would kill it"
    if st.get("app_valid") is not True:
        return f"REFUSED: app_valid is {st.get('app_valid')!r} -- no valid application image to boot ({_fmt_status(st)})"
    try:
        reply = rpc.recovery_exit(resolved)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            return _post_error("/api/recovery/exit", exc)
        reply = {"status": None, "text": f"<reply lost: {exc}>"}
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
    name is kept (the AP passphrase is random per boot anyway).

    REFUSES unless ``confirm is True`` exactly. Reads status first, refuses while the Pico relay is busy.
    Read-back: the board must drop off and answer again as the recovery image
    (a restart actually happened). The status route does not expose Wi-Fi
    credential state, so the clear itself cannot be read back -- the result
    says so. After the reset the board may come up only on its AP (192.168.4.1),
    so an unanswered host is reported UNVERIFIED, never ok.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_wifi_reset (erases the stored home Wi-Fi credentials and restarts)")
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("busy"):
        return f"REFUSED: the Pico relay is busy ({_fmt_pico(pico)}); a restart would kill it"
    try:
        reply = rpc.recovery_wifi_reset(resolved)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            return _post_error("/api/recovery/wifi_reset", exc)
        reply = {"status": None, "text": f"<reply lost: {exc}>"}
    verdict, detail = _poll_restart(resolved, wait_s)
    if verdict == "recovery_again" and reply["status"] is None:
        return (f"UNVERIFIED: the wifi_reset reply was lost ({reply['text']}) though the board restarted "
                f"back into the recovery image ({detail}); whether the credentials were cleared is "
                f"unknown (host={resolved})")
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

    REFUSES unless ``confirm is True`` exactly. Reads recovery status first (``record_present``/
    ``boot_count``), then always POSTs: the status route reports only the
    CURRENT record location, while the board also erases the legacy
    "boot_guard"/"count" record that the main app's boot_guard.c still reads
    as a fallback, so an absent current record does not mean nothing is left.
    The board answers 200 only after reading BOTH locations back absent.
    Re-reads status afterward and FAILS LOUDLY if ``record_present`` is still
    not false -- a 200 reply is not trusted alone, and a lost reply is
    UNVERIFIED.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_boot_guard_reset")
    resolved = _resolve_host(host)
    before, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    try:
        reply = rpc.recovery_boot_guard_reset(resolved)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            return _post_error("/api/ota/esp/boot_guard_reset", exc)
        return (f"UNVERIFIED: POST /api/ota/esp/boot_guard_reset was sent but the reply was lost ({exc}); "
                f"the board may or may not have cleared it -- read recovery_status (host={resolved})")
    try:
        after = rhc.get_status(resolved)
    except rhc.RecoveryHttpError as exc:
        return (f"UNVERIFIED: board replied {reply['text']!r} but the confirming status read failed "
                f"(host={resolved}): {exc}")
    if after.get("record_present") is False:
        return (f"ok - board replied {reply['text']!r} (it erases and reads back both record locations) and "
                f"status confirms: before record_present={before.get('record_present')!r} boot_count="
                f"{before.get('boot_count', 'n/a')}, after record_present=False (host={resolved})")
    return (f"FAILED: board replied {reply['text']!r} but status still shows record_present="
            f"{after.get('record_present')!r} boot_count={after.get('boot_count', 'n/a')} -- "
            f"not cleared (host={resolved})")


@_srv._tool()
def recovery_pico_upload(image_path: str, confirm: bool = False, slot: Optional[str] = None,
                         host: Optional[str] = None, wait_s: float = 600.0) -> str:
    """Flash a SaftyFW slot image (.bin) onto the RP2040 through the recovery
    image's UART relay (POST /api/recovery/pico/upload?crc=<hex>[&slot=A|B],
    -- the CRC32 is computed
    here from the file; `slot` is the operator's target-slot assertion, None
    for auto). The board validates size/vectors/CRC (422 on a bad image) and
    answers 202 when the relay starts; the OUTCOME is only in the status poll.

    REFUSES unless ``confirm is True`` exactly, the file is an existing
    absolute path. Reads both
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
      idle after the 202 (board restarted)     -> UNKNOWN (never ok)
      upload reply lost (timeout/reset)        -> keeps polling so the relay
                         is not abandoned, then UNKNOWN (or FAILED if the
                         relay is idle, i.e. it never started)
    "done" with bytes_sent short of the image is re-read up to DONE_REREADS
    times first (publish_done() updates the two fields non-atomically).
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
    crc = zlib.crc32(image) & 0xFFFFFFFF
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("psram") is not True:
        return f"REFUSED: the board reports no PSRAM for the relay ({_fmt_pico(pico)})"
    if pico.get("busy") is not False:
        return f"REFUSED: the Pico relay is not idle ({_fmt_pico(pico)}); abort or wait first"
    lost_reply = ""
    try:
        reply = rpc.recovery_pico_upload(resolved, image, crc, slot=slot)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            return _post_error("/api/recovery/pico/upload", exc)
        # The board may have taken the image and started the relay. Keep
        # polling (the relay aborts when nobody polls for RPP_CLIENT_GONE_MS)
        # and report whatever is seen as UNKNOWN, never ok.
        lost_reply = str(exc)
    if not lost_reply:
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
        if phase == "idle" and last.get("busy") is False:
            # After a 202 the relay is never idle again unless the board
            # restarted; after a lost reply it means the upload never started.
            if lost_reply:
                return (f"FAILED: the upload reply was lost ({lost_reply}) and the relay is idle -- the "
                        f"upload did not start ({_fmt_pico(last)})")
            return (f"UNKNOWN: relay reports idle after it had started -- the board probably restarted "
                    f"mid-transfer; NOT success, check the Pico by hand ({_fmt_pico(last)})")
        if phase in rhc.PICO_TERMINAL_PHASES:
            break
        if _monotonic() >= deadline:
            return (f"UNKNOWN: relay still running after {wait_s:g}s ({_fmt_pico(last)}) -- NOT success; "
                    f"poll recovery_status")
    prefix = f"crc32={crc:08x} slot={slot or 'auto'} (host={resolved})"
    if lost_reply:
        return (f"UNKNOWN: the upload reply was lost ({lost_reply}); the relay then reported phase={phase!r}, "
                f"which may be this upload's outcome or an earlier one -- NOT success, check the Pico "
                f"version by hand. {_fmt_pico(last)}; {prefix}")
    if phase == "done":
        rereads = 0
        while (rereads < DONE_REREADS and last.get("phase") == "done"
               and not last.get("bytes_sent") == last.get("total_bytes") == len(image)):
            rereads += 1
            _sleep(0.5)
            try:
                last = rhc.get_pico_status(resolved)
            except rhc.RecoveryHttpError:
                continue
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


@_srv._tool()
def recovery_pico_abort(confirm: bool = False, host: Optional[str] = None, wait_s: float = 30.0) -> str:
    """Ask the recovery image's Pico UART relay to stop (POST
    /api/recovery/pico/abort).
    The relay sends ABORT to the Pico and ends in phase "aborted"; the POST's
    plain-text reply ("abort requested") is only an acknowledgement.

    REFUSES unless ``confirm is True`` exactly. Reads both status routes first; if the relay is not busy
    there is nothing to abort and NO POST is sent (reported, not an error).
    After the POST it polls /api/recovery/pico/status for a terminal phase:
      aborted         -> "ok", unless the error text says the relay aborted
                         itself ("browser stopped polling"): then UNVERIFIED
      done           -> NOT aborted: the transfer finished first (reported)
      failed          -> the relay had already failed (reported)
      outcome_unknown -> UNKNOWN (END was sent, result lost; check the Pico)
      still running at `wait_s` / contact lost / reply lost with no terminal
      phase seen      -> UNKNOWN (never ok)
    Aborting a transfer can leave the Pico slot partially written; re-run
    recovery_pico_upload afterward.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_pico_abort (stops an in-flight Pico transfer)")
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("busy") is not True:
        return (f"nothing to abort: the Pico relay is not busy ({_fmt_pico(pico)}); no POST sent "
                f"(host={resolved})")
    lost_reply = ""
    try:
        reply = rpc.recovery_pico_abort(resolved)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            return _post_error("/api/recovery/pico/abort", exc)
        lost_reply = str(exc)
        reply = {"status": None, "text": f"<reply lost: {exc}>"}
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
                return (f"UNKNOWN: lost contact with the board after the abort request ({exc}); last "
                        f"status: {_fmt_pico(last) or 'none'} -- NOT confirmed aborted")
            continue
        phase = last.get("phase")
        if phase in rhc.PICO_TERMINAL_PHASES:
            break
        if phase == "idle" and last.get("busy") is False:
            return (f"UNKNOWN: relay reports idle after the abort request (board restarted?) -- "
                    f"NOT confirmed aborted ({_fmt_pico(last)})")
        if _monotonic() >= deadline:
            return (f"UNKNOWN: relay still running {wait_s:g}s after the abort request ({_fmt_pico(last)}) "
                    f"-- NOT confirmed aborted")
    if phase == "aborted":
        if RELAY_SELF_ABORT_TEXT in str(last.get("error", "")):
            return (f"UNVERIFIED: relay reports aborted, but its error text says the RELAY aborted itself "
                    f"({last.get('error')!r}), not necessarily in answer to this POST (reply "
                    f"{reply['text']!r}); the transfer is stopped either way, but this abort request is "
                    f"not proven to be the cause ({_fmt_pico(last)}) (host={resolved})")
        if lost_reply:
            return (f"UNKNOWN: the abort reply was lost ({lost_reply}) but the relay now reports "
                    f"aborted ({_fmt_pico(last)}); likely this abort, not proven (host={resolved})")
        return f"ok - relay reports aborted after {reply['text']!r}; {_fmt_pico(last)} (host={resolved})"
    if phase == "outcome_unknown":
        return (f"UNKNOWN: relay ended OUTCOME UNKNOWN (END already sent, result lost) -- NOT aborted; "
                f"power-cycle and check the Pico version. {_fmt_pico(last)} (host={resolved})")
    if phase == "done":
        return (f"NOT ABORTED: the transfer completed before the abort took effect "
                f"({_fmt_pico(last)}) (host={resolved})")
    return (f"NOT ABORTED: relay had already ended in phase=failed ({_fmt_pico(last)}) "
            f"(host={resolved})")


@_srv._tool()
def recovery_sw_reset(confirm: bool = False, host: Optional[str] = None, wait_s: float = 60.0) -> str:
    """Software-reset the board while it runs the recovery image (POST
    /api/sw_reset on the RECOVERY image, context "sw-reset"; not the main
    app's route of the same path). The board answers "resetting" and calls
    esp_restart().

    REFUSES unless ``confirm is True`` exactly. Reads status first, refuses while the Pico relay is busy
    (the board 409s and a restart would kill the transfer). Read-back: the
    board must drop off and answer again. Back as the recovery image -> ok;
    answering as the normal application (recovery routes 404) -> ok with that
    stated (the bootloader chose `app`); a lost reply or a board that went
    silent -> UNKNOWN/UNVERIFIED, never ok; never dropped -> FAILED.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_sw_reset (restarts the board)")
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    if pico.get("busy"):
        return f"REFUSED: the Pico relay is busy ({_fmt_pico(pico)}); a restart would kill it"
    try:
        reply = rpc.recovery_sw_reset(resolved)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            return _post_error("/api/sw_reset", exc)
        reply = {"status": None, "text": f"<reply lost: {exc}>"}
    verdict, detail = _poll_restart(resolved, wait_s)
    lost = reply["status"] is None
    if verdict in ("recovery_again", "app"):
        where = (f"back into the recovery image ({detail})" if verdict == "recovery_again"
                 else f"and the host now answers as the normal application ({detail})")
        if lost:
            return (f"UNKNOWN: the sw_reset reply was lost ({reply['text']}) though the board restarted "
                    f"{where} (host={resolved})")
        return f"ok - board replied {reply['text']!r} and restarted {where} (host={resolved})"
    if verdict == "never_restarted":
        return (f"FAILED: board replied {reply['text']!r} but never restarted within {wait_s:g}s and "
                f"still answers as the recovery image (host={resolved})")
    return (f"UNVERIFIED: board replied {reply['text']!r} and went silent; nothing answered at {resolved} "
            f"by {wait_s:g}s -- check by hand")


@_srv._tool()
def recovery_push_esp_image(image_path: str, confirm: bool = False, host: Optional[str] = None,
                            wait_s: float = 90.0) -> str:
    """Push a new ESP application image (KilnCtrl.bin) into the `app`
    partition through the recovery image (POST /api/ota/esp on the RECOVERY
    image, context "esp"). The board validates the first chunk (image header,
    project name, size), streams to flash, verifies the whole image, sets the
    boot partition, clears boot_guard and restarts into it.

    REFUSES unless ``confirm is True`` exactly, the file is an existing
    absolute path whose size is 1..the board's reported ``max_upload`` (the
    `app` partition size, from GET /api/recovery/status) and which starts
    with the ESP image magic byte 0xE9
    only). Reads both status routes first and REFUSES while the Pico relay is
    busy (the board 409s). Verification: after the POST the board must drop
    off and the recovery routes must answer 404 (the application is up) -> ok.
    Back as the recovery image, or never restarted -> FAILED. A reply lost
    mid-upload, or a board silent by `wait_s` -> UNKNOWN/UNVERIFIED, never ok
    (read recovery_status: app_valid says whether the image landed).
    This does NOT prove the new application is healthy beyond answering HTTP.
    A 200 reply that does not say boot_guard was cleared and verified is
    reported "ok-with-warning". A board-reported failure past the busy
    gates (400/422/500...) FAILED message warns that `app` may be partly erased
    and includes a fresh app_valid read.
    """
    if confirm is not True:
        return _refuse_unconfirmed("recovery_push_esp_image (rewrites the application partition and reboots)")
    if not isinstance(image_path, str) or not os.path.isabs(image_path):
        return "REFUSED: image_path must be an absolute path"
    try:
        size = os.path.getsize(image_path)
        if size <= 0:
            return f"REFUSED: image is {size} bytes"
        with open(image_path, "rb") as fh:
            image = fh.read()
    except OSError as exc:
        return f"REFUSED: cannot read image_path: {exc}"
    if image[0] != ESP_IMAGE_MAGIC:
        return f"REFUSED: image does not start with the ESP image magic 0x{ESP_IMAGE_MAGIC:02x} (got 0x{image[0]:02x})"
    resolved = _resolve_host(host)
    st, pico, err = _preflight(resolved)
    if err:
        return f"error: {err}"
    limit = st.get("max_upload") or st.get("app_size")
    if not isinstance(limit, int) or limit <= 0:
        return f"REFUSED: the board reports no usable app partition size ({_fmt_status(st)})"
    if len(image) > limit:
        return f"REFUSED: image is {len(image)} bytes, larger than the app partition ({limit} bytes)"
    if pico.get("busy"):
        return f"REFUSED: the Pico relay is busy ({_fmt_pico(pico)}); the board would 409 and a restart would kill it"
    try:
        reply = rpc.recovery_push_esp_image(resolved, image, timeout=ESP_PUSH_TIMEOUT_S)
    except rpc.RecoveryPostError as exc:
        if not _reply_lost(exc):
            msg = _post_error("/api/ota/esp", exc)
            if getattr(exc, "stage", "") != "post" or exc.status in _PRE_ERASE_STATUSES:
                # Challenge failure (POST never sent) or refused before the
                # board touched `app`.
                return msg
            return msg + _app_may_be_erased_note(resolved)
        reply = {"status": None, "text": f"<reply lost: {exc}>"}
    verdict, detail = _poll_restart(resolved, wait_s)
    prefix = f"{len(image)} bytes (host={resolved})"
    if reply["status"] is None:
        return (f"UNKNOWN: the upload reply was lost ({reply['text']}); afterward the board is "
                f"'{verdict}' {detail} -- NOT confirmed; read recovery_status (app_valid) -- {prefix}")
    if verdict == "app":
        if BOOT_GUARD_CLEARED_TEXT not in reply["text"]:
            return (f"ok-with-warning - the application is answering ({detail}) but boot_guard was NOT "
                    f"confirmed cleared: {_boot_guard_outcome(reply['text'])!r}; the counter may still "
                    f"push the next boot toward recovery -- read boot_guard_get (the recovery routes are "
                    f"gone now that the application is up, so recovery_boot_guard_reset cannot help); board replied {reply['text']!r}; {prefix}")
        return f"ok - board replied {reply['text']!r} and the application is answering ({detail}); {prefix}"
    if verdict == "recovery_again":
        return (f"FAILED: board replied {reply['text']!r} but came back as the RECOVERY image ({detail}) -- "
                f"the new application did not boot; {prefix}")
    if verdict == "never_restarted":
        return (f"FAILED: board replied {reply['text']!r} but never restarted within {wait_s:g}s and "
                f"still answers as the recovery image; {prefix}")
    return (f"UNVERIFIED: board replied {reply['text']!r} and went silent; nothing answered by "
            f"{wait_s:g}s -- check by hand; {prefix}")
