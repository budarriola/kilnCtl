#!/usr/bin/env python3
"""profile_live_http_client.py -- pure HTTP client for the live profile edit
routes (firmware/KilnFW/App/drivers/http/profiles_live_http.c), used by
mcp_server_profile_live.py's profile_live_* tools.

Wire contract source of truth: profiles_live_http.c itself and
docs/LIVE_PROFILE_EDIT_PLAN.md section 10. Mirrored here, not re-derived:

  GET  /api/profile/live
       -> 200 {"active":bool,"origin_id":uint,"origin_is_builtin":bool,
                "working_id":int (-1 if no working copy),
                "editable_from_segment":uint,"pending_decision":bool,
                "last_refusal":null|{"generation":uint,"result":int,"message":str}}
       last_refusal.generation is profile_executor's RAM-only live-edit
       generation counter (live_profile_generation(), live_profile.c) --
       it only ever appears here, inside a refusal record, never as a
       top-level field. There is no other generation counter on this wire
       surface to track separately.
  GET  /api/profile/live?content=1
       -> 200 {"id":uint,"name":str,"zone_mask":uint,"segment_count":uint,
                "segments":[{"seg_kind":uint,"target_c":float,
                "ramp_c_per_hr":float,"dwell_min":uint,"io_target":uint,
                "io_state":uint,"io_blocking":uint,
                "io_leave_on_at_end":uint}, ...]}
       -> 409 {"ok":false,"error":"no working copy -- fork first"} (or
          "working copy is not readable") if there is no pending working copy.
  POST /api/profile/live/fork  (no body)
       -> 200 {"ok":true,"origin_id":uint,"working_id":uint}
       -> 409 {"ok":false,"error":"no active firing to fork from"} (or the
          board's own live_profile_fork() refusal text, e.g. no free slot).
  POST /api/profile/live  (form body -- same field set profiles_parse_profile_fields()
       accepts for POST /api/profile, MINUS `id`: the live route always
       targets the implicit working slot)
       name, zone_mask, seg_count, then per segment 0..seg_count-1:
       seg%u_kind (optional, default 0 = zone ramp/dwell) and either
       seg%u_target/seg%u_ramp/seg%u_dwell or
       seg%u_io_target/seg%u_io_state/seg%u_io_blocking/seg%u_io_leave_on
       -> 200 {"ok":true,"warnings":[...]}
       -> 400 {"ok":false,"error":"..."} on a bound violation (parse or HARD
          validation failure -- names the offending segment/value/limit) or
          on "fork before editing"/"no active firing"/"body too large".
       -> 409 {"ok":false,"error":"..."} on a window-check violation (the
          candidate would change an already-passed or in-flight segment).
  POST /api/profile/live/decide  (form body: action=discard | action=save_as&name=... |
       action=overwrite&confirm=1)
       discard   -> 200 {"ok":true}
       save_as   -> 200 {"ok":true,"id":uint}         (new profile slot)
       overwrite -> 200 {"ok":true}                    (origin slot overwritten)
       -> 400 {"ok":false,"error":"missing action"/"missing name"/
          "confirm=1 required to overwrite"/a save failure reason}
       -> 403 {"ok":false,"error":"..."} refusing to overwrite a builtin
          origin (live_edit_can_overwrite()'s own refusal text).
       -> 409 {"ok":false,"error":"nothing pending"} if there is no pending
          working copy at all.

Unlike ota_http_client.py's plain-text error bodies, EVERY refusal on this
surface is JSON: {"ok":false,"error":"..."} with the right HTTP status
(profiles_live_http.c's send_err_json() family) -- so this client always
tries to json.loads() a non-2xx body and falls back to the raw text only if
that fails.

Same "stdlib urllib.request, no framework" convention as every other client
in this package; issues every request through http_auth.urlopen() (this
server's ADMIN-tier session/login seam -- see http_auth.py) so a 401 under
web auth logs in once via KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD and
retries, exactly like every other admin-tier tool in this package. No MAC/
HMAC signing is involved here -- that is a separate, OTA-specific AP-password
scheme unrelated to these routes.

Build/test-verified only via a fake HTTP server
(tools/PcTools/tests/test_profile_live_http_client.py) -- no physical board
exercised, same caveat every other client module in this package carries.
"""
from __future__ import annotations

import json
import logging
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth

log = logging.getLogger(__name__)

PROFILE_LIVE_HTTP_TIMEOUT_S = 5.0


class ProfileLiveHttpError(Exception):
    """Raised for any transport or protocol failure talking to the board's
    live-profile-edit endpoints -- unreachable host, a non-2xx response, or a
    response that doesn't parse as the expected JSON. Carries the board's own
    {"ok":false,"error":"..."} message text (if any) as `detail` so a caller
    can surface the specific refusal reason (segment/value/limit named,
    window violation, "fork before editing", ...) rather than a generic
    failure string."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def _error_detail(exc: Exception) -> tuple[Optional[int], str]:
    """Normalize a request exception into (status, detail-text). For an
    HTTPError, tries to pull `.error` out of a JSON body first (the house
    shape every refusal on this surface uses); falls back to the raw text if
    the body isn't JSON."""
    if isinstance(exc, urllib.error.HTTPError):
        try:
            raw = exc.read().decode("utf-8", errors="replace").strip()
        except Exception:
            raw = ""
        detail = raw
        try:
            obj = json.loads(raw)
            if isinstance(obj, dict) and isinstance(obj.get("error"), str):
                detail = obj["error"]
        except Exception:
            pass
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def _get_json(host: str, path: str, timeout: float) -> dict:
    req = urllib.request.Request(_url(host, path), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001 - normalized below
        status, detail = _error_detail(exc)
        raise ProfileLiveHttpError(f"GET {path} failed: {detail}", status, detail) from exc
    try:
        return json.loads(body)
    except Exception as exc:
        raise ProfileLiveHttpError(f"GET {path} response was not valid JSON: {body!r}") from exc


def _post_form(host: str, path: str, fields: dict, timeout: float) -> dict:
    data = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        _url(host, path),
        data=data,
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001 - normalized below
        status, detail = _error_detail(exc)
        raise ProfileLiveHttpError(f"POST {path} failed: {detail}", status, detail) from exc
    try:
        return json.loads(body)
    except Exception as exc:
        raise ProfileLiveHttpError(f"POST {path} response was not valid JSON: {body!r}") from exc


def get_live_status(host: str, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S) -> dict:
    """GET /api/profile/live -- the plan section 10 status object. Never
    raises on a "nothing active"/"no working copy" state: those are ordinary
    fields (active=false, working_id=-1), not error responses."""
    return _get_json(host, "/api/profile/live", timeout)


def get_live_content(host: str, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S) -> dict:
    """GET /api/profile/live?content=1 -- the working copy's profile body.
    Raises ProfileLiveHttpError(status=409) if there is no working copy yet
    (fork first)."""
    return _get_json(host, "/api/profile/live?content=1", timeout)


def fork_live(host: str, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/profile/live/fork -- forks whatever is currently running
    into a new working copy. Raises ProfileLiveHttpError(status=409) if
    nothing is running, or if the fork itself fails (e.g. no free slot)."""
    return _post_form(host, "/api/profile/live/fork", {}, timeout)


def edit_live(host: str, name: str, zone_mask: int, segments: list, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S
              ) -> dict:
    """POST /api/profile/live -- saves `segments` (a list of dicts, each
    shaped like one element of get_live_content()'s "segments" list, using
    the same key names the firmware form parser expects: kind, target,
    ramp, dwell, io_target, io_state, io_blocking, io_leave_on) into the
    working slot. `kind` defaults to 0 (zone ramp/dwell) when omitted.

    Raises ProfileLiveHttpError(status=400) on a bound/parse violation (the
    board's message names the offending segment/value/limit), or
    status=409 on a window violation (the candidate would change an
    already-passed or in-flight segment) -- both delivered as the board's
    own {"ok":false,"error":"..."} text in .detail.
    """
    fields = {"name": name, "zone_mask": str(zone_mask), "seg_count": str(len(segments))}
    for i, seg in enumerate(segments):
        kind = seg.get("kind", 0)
        fields[f"seg{i}_kind"] = str(kind)
        if "target" in seg or "ramp" in seg or "dwell" in seg:
            fields[f"seg{i}_target"] = str(seg.get("target", 0))
            fields[f"seg{i}_ramp"] = str(seg.get("ramp", 0))
            fields[f"seg{i}_dwell"] = str(seg.get("dwell", 0))
        if "io_target" in seg or "io_state" in seg or "io_blocking" in seg or "io_leave_on" in seg:
            fields[f"seg{i}_io_target"] = str(seg.get("io_target", 0))
            fields[f"seg{i}_io_state"] = str(seg.get("io_state", 0))
            fields[f"seg{i}_io_blocking"] = str(seg.get("io_blocking", 0))
            fields[f"seg{i}_io_leave_on"] = str(seg.get("io_leave_on", 0))
    return _post_form(host, "/api/profile/live", fields, timeout)


def decide_live_discard(host: str, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/profile/live/decide action=discard -> {"ok":true}. Raises
    ProfileLiveHttpError(status=409) if there is nothing pending."""
    return _post_form(host, "/api/profile/live/decide", {"action": "discard"}, timeout)


def decide_live_save_as(host: str, name: str, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/profile/live/decide action=save_as&name=... ->
    {"ok":true,"id":uint} (the new profile slot). Raises
    ProfileLiveHttpError(status=400) on a missing/invalid name or a save
    failure, status=409 if there is nothing pending."""
    return _post_form(host, "/api/profile/live/decide", {"action": "save_as", "name": name}, timeout)


def decide_live_overwrite(host: str, timeout: float = PROFILE_LIVE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/profile/live/decide action=overwrite&confirm=1 ->
    {"ok":true} (origin slot overwritten in place). Raises
    ProfileLiveHttpError(status=403) if the origin is a builtin profile
    (never overwritable), status=409 if there is nothing pending. This
    function always sends confirm=1 -- the confirm gate belongs at the MCP
    tool layer (profile_live_decide's own `confirm` parameter), not here."""
    return _post_form(host, "/api/profile/live/decide", {"action": "overwrite", "confirm": "1"}, timeout)
