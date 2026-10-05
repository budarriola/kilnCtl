"""HTTP client for the spare-relay aux-output routes (docs/SPARE_RELAY_ONOFF_PLAN.md,
WP-2; firmware/KilnFW/App/drivers/http/aux_outputs_http.c):

  GET  /api/aux_outputs         JSON {quarantined, enabled_mask, conflict_mask,
                                zones_relay_mask, relays:[{relay, enabled, conflicted,
                                tc_zone (-1 = none), hyst_c, min_on_s, min_off_s}]}
  POST /api/aux_outputs         form: relay=1..4, enabled=0/1 (both required);
                                optional tc_zone/hyst_c/min_on_s/min_off_s, each
                                omit-preserves the stored value. 200 {"ok":true}.
  POST /api/aux_outputs/manual  form: relay=1..4, on=0/1. Idle-only. 200 {"ok":true}.
  POST /api/zones               ONLY the one-shot convert form move_zone_to_aux=Z&confirm=1
                                (see post_move_zone_to_aux): frees ON_OFF zone Z's relay into an
                                aux binding and rewrites every stored profile's rules for Z to the
                                aux target. 200 {"ok":true,"zone","relay","profiles_scanned",
                                "profiles_affected","rules_retargeted"}; 400 bad field/confirm;
                                409 refused, nothing changed; 500 a step failed and was rolled back
                                (plain-text body says whether the rollback was clean).
  GET  /api/profiles, GET /api/profile?id=N   read-only, used to count and verify profile rules.

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


def post_move_zone_to_aux(host: str, zone: int, timeout: float = 30.0) -> dict:
    """POST /api/zones with ONLY ``move_zone_to_aux=<zone>&confirm=1`` -- the firmware's one-shot,
    all-or-nothing convert of an ON_OFF zone to an aux output. Returns the parsed 200 JSON.
    Raises AuxHttpError (status/detail set) on any non-2xx, or when a 200 body is not the expected
    JSON. The write can touch up to 100 profile blobs, hence the longer timeout."""
    text = _post(host, "/api/zones", [("move_zone_to_aux", str(zone)), ("confirm", "1")], timeout)
    try:
        data = json.loads(text)
    except Exception as exc:
        raise AuxHttpError(f"POST /api/zones move_zone_to_aux answered 200 with non-JSON: {text!r}") from exc
    if not isinstance(data, dict) or data.get("ok") is not True:
        raise AuxHttpError(f"POST /api/zones move_zone_to_aux did not answer ok: {text!r}")
    return data


def _get_json(host: str, path: str, timeout: float):
    req = urllib.request.Request(_url(host, path), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise AuxHttpError(f"GET {path} failed: {detail}", status, detail) from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise AuxHttpError(f"GET {path} response was not valid JSON: {text!r}") from exc


def get_stored_profile_rules(host: str, timeout: float = AUX_HTTP_TIMEOUT_S) -> "dict[int, list]":
    """{profile id: [rule dict, ...]} for every stored (non-builtin) profile, from GET /api/profiles
    then GET /api/profile?id=N. Each rule carries "zone" (0..2 a zone, 8..11 aux relay 1..4),
    "segment", "temp_source", ..."""
    listing = _get_json(host, "/api/profiles", timeout)
    if not isinstance(listing, list):
        raise AuxHttpError(f"GET /api/profiles was not a JSON array: {listing!r}")
    out: "dict[int, list]" = {}
    for item in listing:
        if not isinstance(item, dict) or item.get("builtin") or not isinstance(item.get("id"), int):
            continue
        pid = item["id"]
        detail = _get_json(host, f"/api/profile?id={pid}", timeout)
        rules = detail.get("on_off_rules") if isinstance(detail, dict) else None
        if not isinstance(rules, list):
            raise AuxHttpError(f"GET /api/profile?id={pid} has no on_off_rules array")
        out[pid] = rules
    return out
