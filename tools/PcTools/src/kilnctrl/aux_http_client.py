"""HTTP client for the spare-relay aux-output routes (docs/SPARE_RELAY_ONOFF_PLAN.md,
WP-2; firmware/KilnFW/App/drivers/http/aux_outputs_http.c):

  GET  /api/aux_outputs         JSON {quarantined, enabled_mask, conflict_mask,
                                zones_relay_mask, relays:[{relay, enabled, conflicted,
                                tc_zone (-1 = none), hyst_c, min_on_s, min_off_s}]}
  POST /api/aux_outputs         form: relay=1..4, enabled=0/1 (both required);
                                optional tc_zone/hyst_c/min_on_s/min_off_s, each
                                omit-preserves the stored value. 200 {"ok":true}.
  POST /api/aux_outputs/manual  form: relay=1..4, on=0/1. Idle-only. 200 {"ok":true}.

All ADMIN tier: goes through the http_auth.urlopen() session seam, so no credential
is handled, printed or logged here. Non-2xx bodies are plain text, surfaced in
AuxHttpError.detail. 409 is either the system_mode_gate refusal (see
zones_http_client.is_system_mode_gate_refusal), a zone conflict, a quarantined store,
or "not an enabled aux output".
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth
from .zones_http_client import _http_error_detail

AUX_HTTP_TIMEOUT_S = 8.0
AUX_RELAY_COUNT = 4


class AuxHttpError(Exception):
    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def get_aux_outputs(host: str, timeout: float = AUX_HTTP_TIMEOUT_S) -> dict:
    req = urllib.request.Request(_url(host, "/api/aux_outputs"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise AuxHttpError(f"GET /api/aux_outputs failed: {detail}", status, detail) from exc
    try:
        data = json.loads(text)
    except Exception as exc:
        raise AuxHttpError(f"GET /api/aux_outputs response was not valid JSON: {text!r}") from exc
    if not isinstance(data, dict) or not isinstance(data.get("relays"), list):
        raise AuxHttpError(f"GET /api/aux_outputs response has no 'relays' array: {text!r}")
    return data


def aux_entry(snapshot: dict, relay: int) -> Optional[dict]:
    for e in snapshot.get("relays") or []:
        if isinstance(e, dict) and e.get("relay") == relay:
            return e
    return None


def _post(host: str, path: str, fields: "list[tuple[str, str]]", timeout: float) -> str:
    data = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        _url(host, path), data=data, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded",
                 "Content-Length": str(len(data))})
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise AuxHttpError(f"POST {path} refused: HTTP {status}: {detail}", status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise AuxHttpError(f"POST {path} unreachable: {detail}") from exc


def _is_ok(text: str) -> bool:
    try:
        return json.loads(text).get("ok") is True
    except Exception:
        return False


def post_aux_output(host: str, relay: int, enabled: bool, tc_zone: Optional[int] = None,
                    hyst_c: Optional[float] = None, min_on_s: Optional[int] = None,
                    min_off_s: Optional[int] = None, timeout: float = AUX_HTTP_TIMEOUT_S) -> bool:
    """POST /api/aux_outputs. Returns True iff the board answered {"ok":true}.
    Fields left None are OMITTED so the firmware preserves the stored value."""
    fields = [("relay", str(relay)), ("enabled", "1" if enabled else "0")]
    if tc_zone is not None:
        fields.append(("tc_zone", str(tc_zone)))
    if hyst_c is not None:
        fields.append(("hyst_c", repr(float(hyst_c))))
    if min_on_s is not None:
        fields.append(("min_on_s", str(min_on_s)))
    if min_off_s is not None:
        fields.append(("min_off_s", str(min_off_s)))
    return _is_ok(_post(host, "/api/aux_outputs", fields, timeout))


def post_aux_manual(host: str, relay: int, on: bool, timeout: float = AUX_HTTP_TIMEOUT_S) -> bool:
    """POST /api/aux_outputs/manual. Returns True iff the board answered {"ok":true}."""
    return _is_ok(_post(host, "/api/aux_outputs/manual",
                        [("relay", str(relay)), ("on", "1" if on else "0")], timeout))
