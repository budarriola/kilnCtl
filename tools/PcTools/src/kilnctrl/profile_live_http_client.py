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
       targets the implicit working slot. Note this form NEVER sends
       rule%u_* fields -- profiles_parse_profile_fields() treats their
       absence as on_off_rule_count=0, so a live edit unconditionally
       erases any ON_OFF zone rules on the working copy. This is
       pre-existing firmware/page behaviour, not something this client
       introduces or can route around.)
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
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth

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
    shaped like one element of get_live_content()'s "segments" list) into the
    working slot. `kind` defaults to 0 (zone ramp/dwell) when omitted.

    Each segment dict accepts EXACTLY these keys (anything else raises
    ValueError, so a typo or a get_live_content()-shaped key that this
    function doesn't recognize fails loud instead of being silently
    dropped -- see the io-branch footgun this replaced: a missing
    io_leave_on/io_leave_on_at_end used to default to 0 and send the
    board seg%u_io_leave_on=0, turning the relay OFF at segment end even
    when the caller only meant to leave that field unspecified):
      kind
      target or target_c, ramp or ramp_c_per_hr, dwell or dwell_min
      io_target, io_state, io_blocking, io_leave_on or io_leave_on_at_end
    Both spellings are accepted per field specifically so the documented
    read-modify-write round trip (GET ?content=1 -> mutate -> edit_live)
    works without the caller renaming every key first: get_live_content()
    returns target_c/ramp_c_per_hr/dwell_min/io_leave_on_at_end, while the
    firmware form-field convention (and this function's own historical
    kwarg style) is target/ramp/dwell/io_leave_on.

    Raises ProfileLiveHttpError(status=400) on a bound/parse violation (the
    board's message names the offending segment/value/limit), or
    status=409 on a window violation (the candidate would change an
    already-passed or in-flight segment) -- both delivered as the board's
    own {"ok":false,"error":"..."} text in .detail.

    A field the caller did not supply for a given segment is NEVER sent as
    an explicit 0 -- it is simply omitted, so the firmware applies its own
    default or rejection for that field (profiles_edit_http.c). This
    matters concretely: a missing seg%u_io_blocking defaults to 1 (safe/
    blocking) on the firmware side, so sending an explicit 0 there would
    silently turn a blocking I/O segment into a non-blocking one; and
    PROFILE_TARGET_C_MIN/PROFILE_RAMP_C_PER_HR_MIN are both 0.0f, so
    sending an explicit 0 for an omitted target/ramp would be silently
    accepted as "0 C" instead of triggering the firmware's own
    "target_c missing" 400.
    """
    ramp_dwell_keys = {"target", "target_c", "ramp", "ramp_c_per_hr", "dwell", "dwell_min"}
    io_keys = {"io_target", "io_state", "io_blocking", "io_leave_on", "io_leave_on_at_end"}
    accepted_keys = {"kind"} | ramp_dwell_keys | io_keys

    fields = {"name": name, "zone_mask": str(zone_mask), "seg_count": str(len(segments))}
    for i, seg in enumerate(segments):
        unknown = set(seg.keys()) - accepted_keys
        if unknown:
            raise ValueError(
                f"segment {i}: unknown key(s) {sorted(unknown)!r} -- accepted keys are "
                f"{sorted(accepted_keys)!r}"
            )
        kind = seg.get("kind", 0)
        fields[f"seg{i}_kind"] = str(kind)

        if "target" in seg or "target_c" in seg:
            fields[f"seg{i}_target"] = str(seg.get("target", seg.get("target_c")))
        if "ramp" in seg or "ramp_c_per_hr" in seg:
            fields[f"seg{i}_ramp"] = str(seg.get("ramp", seg.get("ramp_c_per_hr")))
        if "dwell" in seg or "dwell_min" in seg:
            fields[f"seg{i}_dwell"] = str(seg.get("dwell", seg.get("dwell_min")))

        if "io_target" in seg:
            fields[f"seg{i}_io_target"] = str(seg["io_target"])
        if "io_state" in seg:
            fields[f"seg{i}_io_state"] = str(seg["io_state"])
        if "io_blocking" in seg:
            fields[f"seg{i}_io_blocking"] = str(seg["io_blocking"])
        if "io_leave_on" in seg or "io_leave_on_at_end" in seg:
            fields[f"seg{i}_io_leave_on"] = str(seg.get("io_leave_on", seg.get("io_leave_on_at_end")))
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
