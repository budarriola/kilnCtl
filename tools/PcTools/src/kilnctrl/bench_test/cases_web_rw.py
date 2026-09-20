"""Wave 2: the WEB suite's read/write round-trip cases (plan doc section
3.7) plus WEB-SEC-03 (auth on/off).

Shape, per the task this wave was cut from: every case here **reads the
current value, writes a test value, reads it back, restores the original
value in `finally`, then reads it back again** -- restoring even on an
exception, never only on the happy path (same discipline as
`cases_heat.py`'s `_cleanup_bench_profile`). The verdict is about the
*write path* (did the test value actually take effect) EXCEPT that a case
FAILs unconditionally if the restore step itself does not round-trip back
to the original value -- leaving the board in the written-to state is a
hazard this suite must never paper over, regardless of whether the write
path under test looked correct.

Credentials for WEB-SEC-03 come only from the ``KILNCTL_WEB_USERNAME`` /
``KILNCTL_WEB_PASSWORD`` environment variables (or the matching ``ctx``
overrides used by tests) -- never persisted, never logged, and never
present anywhere in `summary.json`/`transcript.md` (plan doc section 6
rule 9). WEB-SEC-03 also never enables ``web_enabled`` without first
confirming ``set_web_password``'s response reports ``ok:true`` -- the same
"check the write actually landed before depending on it" bug the wizard fix
in commit 5db79ac8 addressed, applied here to the same
``cmd=set_web_password`` / ``cmd=set_policy`` pair.

All board access is bare HTTP through the existing routes named in the plan
doc (``/api/status``, ``/api/unit_pref``, ``/api/watchdog_cfg``,
``/api/ramp_assist``, ``/api/auth/config``, ``/api/auth/security``,
``/api/auth/login``, ``/api/auth/session``, ``/api/auth/session/extend``,
``/settings/zones``) -- no new HTTP routes, no MCP tool beyond what already
exists. ``ctx["http_get_json"]``/``ctx["http_post_json"]`` let unit tests
replace the transport with a fake sequencer; ``ctx["sec_client"]`` does the
same for WEB-SEC-03's richer client (login/session calls a plain GET/POST
pair does not cover cleanly).
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Optional, Tuple

from . import cases_web
from . import judgments as J
from .registry import CaseResult, Verdict, get_case

_http_get_raw = cases_web._http_get_raw


def _parse_json(text: Optional[str]) -> Optional[dict]:
    if not text:
        return None
    try:
        return json.loads(text)
    except (ValueError, TypeError):
        return None


def _http_post_raw(host: str, path: str, fields: Dict[str, Any], timeout: float = 5.0,
                    cookie: Optional[str] = None) -> "Tuple[Optional[int], Optional[str], Any]":
    """Bare form-encoded POST -- deliberately no session auto-login (unlike
    kilnctrl.http_auth): the round-trip cases run in the "auth off" part of
    the fixed order (plan doc section 5.2) and WEB-SEC-03 is itself testing
    the auth transition, so both need to see exactly what an unauthenticated
    (or explicitly cookie-carrying) client gets."""
    url = f"http://{host}{path}"
    body = urllib.parse.urlencode(fields).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST",
                                  headers={"Content-Type": "application/x-www-form-urlencoded"})
    if cookie:
        req.add_header("Cookie", f"kiln_sid={cookie}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace"), resp.headers
    except urllib.error.HTTPError as exc:
        try:
            detail = exc.read().decode("utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            detail = None
        return exc.code, detail, exc.headers
    except (urllib.error.URLError, OSError) as exc:
        return None, str(exc), None


# ---------------------------------------------------------------------------
# Generic GET/POST-JSON seam -- ctx overrides let tests fake the transport
# without touching urllib at all.
# ---------------------------------------------------------------------------

def _get_json(ctx: dict, path: str) -> "Tuple[Optional[int], Optional[dict]]":
    fn = ctx.get("http_get_json")
    if fn is not None:
        return fn(path)
    host = ctx.get("host")
    if not host:
        return None, None
    status, text = _http_get_raw(host, path)
    return status, _parse_json(text)


def _post_json(ctx: dict, path: str, fields: Dict[str, Any]) -> "Tuple[Optional[int], Optional[dict]]":
    fn = ctx.get("http_post_json")
    if fn is not None:
        return fn(path, fields)
    host = ctx.get("host")
    if not host:
        return None, None
    status, text, _headers = _http_post_raw(host, path, fields)
    return status, _parse_json(text)


# ---------------------------------------------------------------------------
# WEB-DASH-13 / WEB-DIAG-07 / WEB-DIAG-08 -- simple GET-current /
# POST-test-value / GET-verify / restore-in-finally / GET-verify round trips.
# ---------------------------------------------------------------------------

def _bool_toggle_case(ctx: dict, get_path: str, post_path: str, get_field: str,
                       post_form_field: str) -> CaseResult:
    status0, body0 = _get_json(ctx, get_path)
    if status0 != 200 or body0 is None or get_field not in body0:
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET {get_path} failed or missing {get_field!r} (status={status0})",
            observed={"status": status0, "body": body0},
        )
    original = bool(body0[get_field])
    test_value = not original
    write_ok = False
    after_write: Optional[bool] = None
    restore_ok = False
    restored: Optional[bool] = None
    try:
        w_status, w_body = _post_json(ctx, post_path, {post_form_field: "1" if test_value else "0"})
        write_ok = w_status == 200 and bool(w_body) and w_body.get("ok") is True
        r_status, r_body = _get_json(ctx, get_path)
        after_write = bool(r_body[get_field]) if r_body and get_field in r_body else None
    finally:
        # Restore runs even on an exception above -- a case that throws
        # mid-write must never leave the board holding the test value.
        rw_status, rw_body = _post_json(ctx, post_path, {post_form_field: "1" if original else "0"})
        restore_ok = rw_status == 200 and bool(rw_body) and rw_body.get("ok") is True
        rr_status, rr_body = _get_json(ctx, get_path)
        restored = bool(rr_body[get_field]) if rr_body and get_field in rr_body else None

    return J.judge_web_rw_toggle(get_field, original, test_value, write_ok, after_write, restore_ok, restored)


def _case_dash13(ctx: dict) -> CaseResult:
    """WEB-DASH-13: ``POST /api/unit_pref F`` then ``/api/status``
    ``temp_unit`` F, restore C (plan doc section 3.7's own wording for this
    case)."""
    status0, body0 = _get_json(ctx, "/api/status")
    if status0 != 200 or body0 is None or body0.get("temp_unit") not in ("C", "F"):
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET /api/status failed or temp_unit missing/invalid (status={status0})",
            observed={"status": status0, "body": body0},
        )
    original = body0["temp_unit"]
    test_value = "F" if original == "C" else "C"
    write_ok = False
    after_write: Optional[str] = None
    restore_ok = False
    restored: Optional[str] = None
    try:
        w_status, w_body = _post_json(ctx, "/api/unit_pref", {"unit": test_value})
        write_ok = w_status == 200 and bool(w_body) and w_body.get("ok") is True
        r_status, r_body = _get_json(ctx, "/api/status")
        after_write = r_body.get("temp_unit") if r_body else None
    finally:
        rw_status, rw_body = _post_json(ctx, "/api/unit_pref", {"unit": original})
        restore_ok = rw_status == 200 and bool(rw_body) and rw_body.get("ok") is True
        rr_status, rr_body = _get_json(ctx, "/api/status")
        restored = rr_body.get("temp_unit") if rr_body else None

    return J.judge_web_rw_toggle("temp_unit", original, test_value, write_ok, after_write, restore_ok, restored)


def _case_diag07(ctx: dict) -> CaseResult:
    """WEB-DIAG-07: watchdog-panic toggle round trip, restored (plan doc
    section 3.7)."""
    return _bool_toggle_case(ctx, "/api/watchdog_cfg", "/api/watchdog_cfg", "panic_disabled", "disabled")


def _case_diag08(ctx: dict) -> CaseResult:
    """WEB-DIAG-08: ramp assist toggle round trip, restored to off (plan doc
    section 3.7)."""
    return _bool_toggle_case(ctx, "/api/ramp_assist", "/api/ramp_assist", "enabled", "enabled")


# ---------------------------------------------------------------------------
# WEB-SEC-03 -- enable web auth with harness credentials, verify the auth
# surface, then disable and confirm PcTools' HTTP reads are un-blinded
# again. This case *always* restores, in finally (plan doc section 3.7).
# ---------------------------------------------------------------------------

class _SecHttpClient:
    """Thin, real-HTTP implementation of the seam ``_case_web_sec03`` needs.
    Tests inject a fake object with the same method names via
    ``ctx["sec_client"]`` instead of touching urllib."""

    def __init__(self, host: str, timeout: float = 8.0) -> None:
        self.host = host
        self.timeout = timeout

    def _get(self, path: str, cookie: Optional[str] = None) -> "Tuple[Optional[int], Optional[str], Any]":
        url = f"http://{self.host}{path}"
        req = urllib.request.Request(url, method="GET")
        if cookie:
            req.add_header("Cookie", f"kiln_sid={cookie}")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.getcode(), resp.read().decode("utf-8", errors="replace"), resp.headers
        except urllib.error.HTTPError as exc:
            try:
                detail = exc.read().decode("utf-8", errors="replace")
            except Exception:  # noqa: BLE001
                detail = None
            return exc.code, detail, exc.headers
        except (urllib.error.URLError, OSError) as exc:
            return None, str(exc), None

    def get_config(self) -> "Tuple[Optional[int], Optional[dict]]":
        status, text, _headers = self._get("/api/auth/config")
        return status, _parse_json(text)

    def set_web_password(self, username: str, password: str) -> "Tuple[Optional[int], Optional[dict]]":
        status, text, _headers = _http_post_raw(self.host, "/api/auth/security", {
            "cmd": "set_web_password", "role": "admin", "username": username, "password": password,
        })
        return status, _parse_json(text)

    def set_policy(self, web_enabled: bool, lcd_enabled: bool, web_timeout_min: int,
                   lcd_timeout_min: int) -> "Tuple[Optional[int], Optional[dict]]":
        status, text, _headers = _http_post_raw(self.host, "/api/auth/security", {
            "cmd": "set_policy",
            "web_enabled": "1" if web_enabled else "0",
            "lcd_enabled": "1" if lcd_enabled else "0",
            "web_timeout_min": str(web_timeout_min),
            "lcd_timeout_min": str(lcd_timeout_min),
        })
        return status, _parse_json(text)

    def login(self, username: str, password: str) -> "Tuple[Optional[int], Optional[str]]":
        status, _text, headers = _http_post_raw(self.host, "/api/auth/login", {
            "username": username, "password": password,
        })
        cookie = None
        if headers is not None:
            for raw in (headers.get_all("Set-Cookie") or []) if hasattr(headers, "get_all") else []:
                name, _, rest = raw.partition("=")
                if name.strip() == "kiln_sid":
                    cookie = rest.split(";", 1)[0].strip()
                    break
        return status, cookie

    def get_session(self, cookie: str) -> "Tuple[Optional[int], Optional[dict]]":
        status, text, _headers = self._get("/api/auth/session", cookie=cookie)
        return status, _parse_json(text)

    def extend_session(self, cookie: str) -> "Tuple[Optional[int], Optional[dict]]":
        status, text, _headers = _http_post_raw(self.host, "/api/auth/session/extend", {}, cookie=cookie)
        return status, _parse_json(text)

    def get_status(self, path: str, cookie: Optional[str] = None) -> Optional[int]:
        status, _text, _headers = self._get(path, cookie=cookie)
        return status


def _sec_client(ctx: dict) -> Any:
    client = ctx.get("sec_client")
    if client is not None:
        return client
    host = ctx.get("host")
    if not host:
        from .. import mcp_server_ota  # local import: avoids importing kilnctrl.mcp_server at module load

        host = mcp_server_ota._ota_resolve_host(None)
    return _SecHttpClient(host)


def _case_web_sec03(ctx: dict) -> CaseResult:
    client = _sec_client(ctx)
    username = ctx.get("web_username") or os.environ.get("KILNCTL_WEB_USERNAME")
    password = ctx.get("web_password") or os.environ.get("KILNCTL_WEB_PASSWORD")

    if not username or not password:
        return CaseResult(
            Verdict.SKIP,
            reason="KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD not set in the environment",
            observed={},
        )

    status0, cfg0 = client.get_config()
    if status0 != 200 or cfg0 is None:
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET /api/auth/config failed (status={status0})",
            observed={"status": status0},
        )

    orig = {
        "web_enabled": bool(cfg0.get("web_enabled")),
        "lcd_enabled": bool(cfg0.get("lcd_enabled")),
        "web_timeout_min": cfg0.get("web_timeout_min") if (cfg0.get("web_timeout_min") or 0) > 0 else 30,
        "lcd_timeout_min": cfg0.get("lcd_timeout_min") if (cfg0.get("lcd_timeout_min") or 0) > 0 else 30,
    }

    pw_ok = False
    enabled_ok = False
    dashboard_ok: Optional[bool] = None
    admin_route_gated: Optional[bool] = None
    login_ok = False
    session_ok = False
    extend_ok = False
    state: Dict[str, Any] = {"orig": orig}

    try:
        # Never enable web_enabled without first confirming the admin
        # password this run is about to log in with is actually stored --
        # the wizard fix in commit 5db79ac8's ordering, applied here.
        pw_status, pw_resp = client.set_web_password(username, password)
        pw_ok = pw_status == 200 and bool(pw_resp) and pw_resp.get("ok") is True
        state["set_web_password_status"] = pw_status
        if not pw_ok:
            state["enable_skipped_reason"] = "set_web_password did not report ok:true"
        else:
            en_status, en_resp = client.set_policy(
                True, orig["lcd_enabled"], orig["web_timeout_min"], orig["lcd_timeout_min"])
            enabled_ok = en_status == 200 and bool(en_resp) and en_resp.get("ok") is True
            state["set_policy_enable_status"] = en_status

            if enabled_ok:
                dashboard_ok = client.get_status("/") == 200
                admin_route_gated = client.get_status("/settings/zones") in (302, 401, 403)
                login_status, cookie = client.login(username, password)
                login_ok = login_status == 200 and bool(cookie)
                state["login_status"] = login_status
                if login_ok:
                    sess_status, sess_body = client.get_session(cookie)
                    session_ok = sess_status == 200 and bool(sess_body)
                    state["session"] = sess_body
                    ext_status, _ext_body = client.extend_session(cookie)
                    extend_ok = ext_status == 200
    finally:
        # Unconditional, even on an exception above: this case must never
        # leave web auth enabled with an unconfirmed credential (plan
        # doc section 5.3 rule 4 -- "WEB-SEC-03's restore runs even on
        # exception").
        restore_status, restore_resp = client.set_policy(
            orig["web_enabled"], orig["lcd_enabled"], orig["web_timeout_min"], orig["lcd_timeout_min"])
        restore_post_ok = restore_status == 200 and bool(restore_resp) and restore_resp.get("ok") is True
        readback_status, cfg_after = client.get_config()
        restore_matches = (
            readback_status == 200 and cfg_after is not None and
            bool(cfg_after.get("web_enabled")) == orig["web_enabled"]
        )
        state["restore"] = {
            "post_status": restore_status, "post_ok": restore_post_ok,
            "readback_status": readback_status, "readback_matches": restore_matches,
        }

    return J.judge_web_sec03(
        pw_ok=pw_ok, enabled_ok=enabled_ok, dashboard_ok=dashboard_ok,
        admin_route_gated=admin_route_gated, login_ok=login_ok, session_ok=session_ok,
        extend_ok=extend_ok, restore_ok=state["restore"]["post_ok"],
        restore_matches=state["restore"]["readback_matches"], state=state,
    )


#: Wire this wave's judge functions into the shared REGISTRY (same
#: convention as cases_web.py's own tail).
_CASE_FUNCS = {
    "WEB-DASH-13": _case_dash13,
    "WEB-DIAG-07": _case_diag07,
    "WEB-DIAG-08": _case_diag08,
    "WEB-SEC-03": _case_web_sec03,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
