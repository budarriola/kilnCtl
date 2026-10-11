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

All board access is HTTP through the existing routes named in the plan doc
(``/api/status``, ``/api/unit_pref``, ``/api/watchdog_cfg``,
``/api/ramp_assist``, ``/api/auth/config``, ``/api/auth/security``,
``/api/auth/login``, ``/api/auth/session``, ``/api/auth/session/extend``,
``/settings/zones``) -- no new HTTP routes, no MCP tool beyond what already
exists. ``/api/unit_pref`` POST, ``/api/watchdog_cfg``, ``/api/ramp_assist``
and ``/api/auth/config``/``/api/auth/security`` are all ROUTE_TIER_ADMIN, so
the reads/writes that exercise them (WEB-DASH-13, WEB-DIAG-07, WEB-DIAG-08,
and WEB-SEC-03's own ``get_config``/``set_web_password``/``set_policy``) go
through ``kilnctrl.http_auth.urlopen`` -- the same seam every other
admin-tier PcTools client authenticates through -- so these cases keep
working whether web auth happens to be on or off on the board when the run
reaches them, rather than 401ing whenever it is already on. The three calls
that are deliberately still bare and unauthenticated are named explicitly at
their call sites: ``_SecHttpClient.login()``/``.extend_session()`` (WEB-SEC-03
is testing the login/session surface itself) and ``.get_status()`` used for
``page_shell_open``/``dashboard_ok`` (testing what an unauthenticated
visitor sees is the point of that check). ``ctx["http_get_json"]``/
``ctx["http_post_json"]`` let unit tests replace the transport with a fake
sequencer; ``ctx["sec_client"]`` does the same for WEB-SEC-03's richer client
(login/session calls a plain GET/POST pair does not cover cleanly).
"""
from __future__ import annotations

import gzip
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from typing import Any, Dict, Optional, Tuple

from . import cases_web
from . import board_lock
from . import judgments as J
from .. import http_auth
from .registry import CaseResult, Verdict, get_case

#: WEB-SEC-03's dashboard-after-enable probe retries this many times with
#: this backoff before failing (2026-09-24 fix): a single-worker httpd
#: (project memory project_httpd_single_worker_static_buffers_by_design) can
#: still be busy moments after the set_policy(web_enabled=1) write that just
#: preceded it.
_DASHBOARD_PROBE_ATTEMPTS = 3
_DASHBOARD_PROBE_BACKOFF_S = 1.5

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
    kilnctrl.http_auth). Used only by ``_SecHttpClient.login()`` and
    ``.extend_session()``: WEB-SEC-03 is itself testing the login/session
    surface, so those two calls need to see exactly what an unauthenticated
    (or explicitly cookie-carrying) client gets, not have this module log in
    on their behalf. Every other admin-tier write in this file goes through
    ``_http_post_raw_authed`` instead."""
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


class BodyDecodeError(OSError):
    """Content-Encoding said gzip but the body would not decode: a transport
    problem, never a board verdict. Subclasses OSError so the GET seams map
    it to ``(None, reason)`` (unreachable -> INCONCLUSIVE, not FAIL)."""


def _decode_body(raw: bytes, headers) -> str:
    """Decode a response body, gunzipping when Content-Encoding says gzip."""
    enc = ""
    try:
        enc = (headers.get("Content-Encoding") or "") if headers is not None else ""
    except Exception:  # noqa: BLE001
        enc = ""
    if enc.lower() == "gzip":
        try:
            raw = gzip.decompress(raw)
        except (OSError, EOFError, zlib.error) as exc:
            raise BodyDecodeError(f"gzip decode failed: {exc}") from exc
    return raw.decode("utf-8", errors="replace")


def _http_get_raw_authed(host: str, path: str, timeout: float = 5.0) -> "Tuple[Optional[int], Optional[str]]":
    """Authenticated GET via ``kilnctrl.http_auth.urlopen`` -- for
    WEB-DASH-13/WEB-DIAG-07/WEB-DIAG-08 and WEB-SEC-03's own
    ``/api/auth/config`` reads, all of which are ROUTE_TIER_ADMIN. Unlike
    ``_http_get_raw``/``_http_post_raw`` above (deliberately bare, used only
    where a case needs to see exactly what an unauthenticated or
    explicit-cookie client gets), these round-trip cases are testing the
    write path itself, not the auth gate, and must keep working whether the
    board's web auth happens to be on or off when the run reaches them."""
    url = f"http://{host}{path}"
    # urllib's default "Accept-Encoding: identity" makes the firmware answer
    # 406 for its gzip-only embedded pages; advertise gzip and decode.
    req = urllib.request.Request(url, method="GET", headers={"Accept-Encoding": "gzip"})
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), _decode_body(resp.read(), resp.headers)
    except urllib.error.HTTPError as exc:
        try:
            detail = _decode_body(exc.read(), exc.headers) if exc.fp else None
        except Exception:  # noqa: BLE001
            # Includes BodyDecodeError: the HTTP status is real and decisive
            # (a 409/403 refusal); only the body is unusable.
            detail = None
        return exc.code, detail
    except (urllib.error.URLError, OSError, http_auth.HttpAuthError) as exc:
        return None, str(exc)


def _http_post_raw_authed(host: str, path: str, fields: Dict[str, Any],
                           timeout: float = 5.0) -> "Tuple[Optional[int], Optional[str]]":
    """Authenticated POST counterpart to :func:`_http_get_raw_authed` -- see
    that function's docstring."""
    _check_write_allowed(path, fields)
    url = f"http://{host}{path}"
    body = urllib.parse.urlencode(fields).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST",
                                  headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        try:
            detail = exc.read().decode("utf-8", errors="replace") if exc.fp else None
        except Exception:  # noqa: BLE001
            detail = None
        return exc.code, detail
    except (urllib.error.URLError, OSError, http_auth.HttpAuthError) as exc:
        return None, str(exc)


# ---------------------------------------------------------------------------
# Generic GET/POST-JSON seam -- ctx overrides let tests fake the transport
# without touching urllib at all. Real transport goes through
# kilnctrl.http_auth (same seam every other admin-tier PcTools client uses)
# so WEB-DASH-13/WEB-DIAG-07/WEB-DIAG-08 keep working whether or not web
# auth happens to be enabled on the board when the run reaches them.
# ---------------------------------------------------------------------------

def _get_json(ctx: dict, path: str) -> "Tuple[Optional[int], Optional[dict]]":
    fn = ctx.get("http_get_json")
    if fn is not None:
        return fn(path)
    host = ctx.get("host")
    if not host:
        return None, None
    status, text = _http_get_raw_authed(host, path)
    return status, _parse_json(text)


#: Wi-Fi provisioning and credential-clearing writes no WEB case may ever make
#: (plan doc BENCH_TEST_WEB_JUDGES_PLAN section 3 item 5; BENCH_TEST_SYSTEM_PLAN
#: section 6 rule 4). Path suffixes are matched on the query-stripped path; the
#: field entry is (name, value) matched against form fields / a JSON body.
_WIFI_WRITE_DENYLIST = {
    "path_suffixes": ("/provision", "/forget", "/ip_config"),
    "fields": (("cmd", "clear_credentials"),),
}


class ForbiddenWrite(RuntimeError):
    """Raised by every WEB POST seam for a deny-listed Wi-Fi/credential write."""


def _check_write_allowed(path: str, fields: Any) -> None:
    bare = path.split("?", 1)[0].rstrip("/")
    for suffix in _WIFI_WRITE_DENYLIST["path_suffixes"]:
        if bare.endswith(suffix):
            raise ForbiddenWrite(f"POST {path} is on the Wi-Fi/credential write deny-list")
    if isinstance(fields, dict):
        for name, value in _WIFI_WRITE_DENYLIST["fields"]:
            if str(fields.get(name)) == value:
                raise ForbiddenWrite(f"POST {path} with {name}={value} is on the credential write deny-list")


def _post_json(ctx: dict, path: str, fields: Dict[str, Any]) -> "Tuple[Optional[int], Optional[dict]]":
    _check_write_allowed(path, fields)
    fn = ctx.get("http_post_json")
    if fn is not None:
        status, body = fn(path, fields)
        _note_post_status(ctx, status)
        return status, body
    host = ctx.get("host")
    if not host:
        return None, None
    status, text = _http_post_raw_authed(host, path, fields)
    _note_post_status(ctx, status)
    return status, _parse_json(text)


def _get_text(ctx: dict, path: str) -> "Tuple[Optional[int], Optional[str]]":
    """Authed text GET (e.g. /api/history.csv). Fake: ctx["http_get_text"]."""
    fn = ctx.get("http_get_text")
    if fn is not None:
        return fn(path)
    host = ctx.get("host")
    if not host:
        return None, None
    return _http_get_raw_authed(host, path)


def _post_raw(ctx: dict, path: str, fields: Dict[str, Any]) -> "Tuple[Optional[int], Optional[str]]":
    """Authed form POST returning the raw reply text (plain ``ok`` replies).
    Fake: ctx["http_post_raw"]."""
    _check_write_allowed(path, fields)
    fn = ctx.get("http_post_raw")
    if fn is not None:
        res = fn(path, fields)
    else:
        host = ctx.get("host")
        if not host:
            return None, None
        res = _http_post_raw_authed(host, path, fields)
    _note_post_status(ctx, res[0])
    return res


def _http_post_json_body_authed(host: str, path: str, body: Any,
                                timeout: float = 5.0) -> "Tuple[Optional[int], Optional[str]]":
    url = f"http://{host}{path}"
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                  headers={"Content-Type": "application/json"})
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        try:
            detail = exc.read().decode("utf-8", errors="replace") if exc.fp else None
        except Exception:  # noqa: BLE001
            detail = None
        return exc.code, detail
    except (urllib.error.URLError, OSError, http_auth.HttpAuthError) as exc:
        return None, str(exc)


def _post_json_body(ctx: dict, path: str, body: Any) -> "Tuple[Optional[int], Optional[str]]":
    """Authed JSON-body POST (/api/profile/import). Fake: ctx["http_post_json_body"]."""
    _check_write_allowed(path, body)
    fn = ctx.get("http_post_json_body")
    if fn is not None:
        res = fn(path, body)
    else:
        host = ctx.get("host")
        if not host:
            return None, None
        res = _http_post_json_body_authed(host, path, body)
    _note_post_status(ctx, res[0])
    return res


# ---------------------------------------------------------------------------
# Shared gates (web judge contract audit 2, M1 / L1 / L10).
# ---------------------------------------------------------------------------

_CTX_POST_503 = "_post_503_count"


def _note_post_status(ctx: dict, status: Any) -> None:
    """Count 503 replies on the WEB POST seams: the cfg LittleFS partition
    being unmounted answers 503 (cfg_fs_http_refuse_if_unmounted), a board
    state and not a contract failure of the route under test."""
    if status == 503:
        ctx[_CTX_POST_503] = int(ctx.get(_CTX_POST_503, 0) or 0) + 1


def cfg_unmounted_reason(ctx: dict) -> Optional[str]:
    """A reason string when ``GET /api/cfgfs`` says the cfg partition is not
    mounted, else None (mounted, or unreadable: a 503 on the write is mapped
    to INCONCLUSIVE separately by :func:`cfg_guarded`)."""
    try:
        status, body = _get_json(ctx, "/api/cfgfs")
    except Exception:  # noqa: BLE001
        return None
    if status == 200 and isinstance(body, dict) and body.get("mounted") is False:
        return "cfg partition not mounted (board state, e.g. a format is pending); no write attempted"
    return None


def cfg_guarded(fn):
    """Wrap a judge that writes a cfg-backed route (M1). Before the judge runs
    an unmounted cfg partition gives INCONCLUSIVE with nothing written; after
    it, a FAIL while any POST answered 503 is re-labelled INCONCLUSIVE (the
    board refused because cfg is unmounted), except a reason starting
    "ERROR:" (a possible leftover) which stays FAIL."""
    def wrapper(ctx: dict) -> CaseResult:
        why = cfg_unmounted_reason(ctx)
        if why:
            return CaseResult(Verdict.INCONCLUSIVE, reason=why, observed={"cfgfs_mounted": False})
        ctx[_CTX_POST_503] = 0
        result = fn(ctx)
        n503 = int(ctx.get(_CTX_POST_503, 0) or 0)
        if result.verdict == Verdict.FAIL and n503 and not (result.reason or "").startswith("ERROR:"):
            obs = dict(result.observed or {})
            obs["post_503_count"] = n503
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=f"{n503} POST(s) answered 503 (cfg partition unmounted); not a route verdict: {result.reason}",
                observed=obs,
            )
        return result
    wrapper.__name__ = getattr(fn, "__name__", "wrapper")
    wrapper.__doc__ = getattr(fn, "__doc__", None)
    return wrapper


def idle_gate_reason(ctx: dict) -> Optional[str]:
    """Executor must read idle and autotune idle/done/aborted (L1): the same
    rule as the dash/diag/misc modules' gate. Returns a reason or None."""
    st, ex = _get_json(ctx, "/api/profile_exec")
    if st != 200 or not isinstance(ex, dict):
        return f"GET /api/profile_exec unreadable (status={st}); cannot confirm the executor is idle"
    if ex.get("state") != "idle":
        return f"profile executor is {ex.get('state')!r}, not idle"
    st, at = _get_json(ctx, "/api/autotune")
    if st != 200 or not isinstance(at, dict):
        return f"GET /api/autotune unreadable (status={st}); cannot confirm autotune is inactive"
    if at.get("state") not in ("idle", "done", "aborted"):
        return f"autotune is active (state={at.get('state')!r})"
    return None


def policy_unstored(cfg: Optional[dict]) -> bool:
    """True when GET /api/auth/config shows the all-defaults snapshot the
    handler prints when NO policy was ever stored (false/false/-1/-1,
    security_http.c config handler). set_policy is the only HTTP writer and
    there is no clear-policy route, so a restore could never put "none" back
    (L10); judges that restore a policy refuse to start from this state."""
    return (isinstance(cfg, dict) and not cfg.get("web_enabled") and not cfg.get("lcd_enabled")
            and cfg.get("web_timeout_min") == -1 and cfg.get("lcd_timeout_min") == -1)


# ---------------------------------------------------------------------------
# WEB-DASH-13 / WEB-DIAG-07 / WEB-DIAG-08 -- simple GET-current /
# POST-test-value / GET-verify / restore-in-finally / GET-verify round trips.
# ---------------------------------------------------------------------------

def _bool_toggle_case(ctx: dict, get_path: str, post_path: str, get_field: str,
                       post_form_field: str) -> CaseResult:
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
    idle = idle_gate_reason(ctx)
    if idle:
        return CaseResult(Verdict.SKIP, reason=f"gate: {idle}; no write attempted")
    status0, body0 = _get_json(ctx, get_path)
    if status0 is None or status0 == 401:
        # L5: a transport/session failure on the first read is not a verdict
        # on the route under test.
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason=f"GET {get_path} unreachable or unauthorised (status={status0}); no write attempted",
                          observed={"status": status0})
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
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
    idle = idle_gate_reason(ctx)
    if idle:
        return CaseResult(Verdict.SKIP, reason=f"gate: {idle}; no write attempted")
    status0, body0 = _get_json(ctx, "/api/status")
    if status0 is None or status0 == 401:
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason=f"GET /api/status unreachable or unauthorised (status={status0}); no write attempted",
                          observed={"status": status0})
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

#: Reuse cases_web.py's own no-redirect handler (its 2026-09-21 WEB-X-03 fix
#: for the exact same firmware behaviour: an unauthenticated GET on a gated
#: PAGE route answers "302 Found -> /login?return=<uri>"
#: (http_auth_http.c:307-334), not a 401/403 body -- plain urlopen() follows
#: it and lands on the login page with 200. _SecHttpClient had the same bug
#: independently, since it never shared cases_web.py's opener.
_NoRedirectHandler = cases_web._NoRedirect


def _cookie_attr_flags(set_cookie_rest: str) -> Dict[str, Any]:
    """Attribute flags of a Set-Cookie header (the part after ``name=``).
    The value is dropped here and never stored or logged. Firmware sends
    ``kiln_sid=<hex>; HttpOnly; SameSite=Strict; Path=/``
    (web_auth_login_http.c:556)."""
    attrs = [a.strip() for a in set_cookie_rest.split(";")[1:]]
    low = [a.lower() for a in attrs]
    same = next((a.split("=", 1)[1] for a in attrs if a.lower().startswith("samesite=") and "=" in a), None)
    return {"httponly": "httponly" in low, "secure": "secure" in low, "samesite": same}


class _SecHttpClient:
    """Thin, real-HTTP implementation of the seam ``_case_web_sec03`` needs.
    Tests inject a fake object with the same method names via
    ``ctx["sec_client"]`` instead of touching urllib."""

    def __init__(self, host: str, timeout: float = 8.0) -> None:
        self.host = host
        self.timeout = timeout
        # A per-instance opener (not urllib.request's module-global default)
        # so this class never auto-follows a redirect -- see
        # _NoRedirectHandler above. Tests patch ``client._opener.open``
        # rather than ``urllib.request.urlopen``.
        self._opener = urllib.request.build_opener(_NoRedirectHandler)
        #: Attribute flags of the last login's kiln_sid Set-Cookie, e.g.
        #: {"httponly": True, "samesite": "Strict", "secure": False}. Never the
        #: cookie value. None until a login returned a kiln_sid cookie.
        self.last_login_set_cookie: "Optional[Dict[str, Any]]" = None

    def _get(self, path: str, cookie: Optional[str] = None) -> "Tuple[Optional[int], Optional[str], Any]":
        url = f"http://{self.host}{path}"
        # Advertise gzip like a real browser (and like curl, which offers
        # "Accept-Encoding: gzip, deflate" by default) -- urllib's own
        # default is "Accept-Encoding: identity", which
        # web_client_accepts_gzip() reads as an explicit exclusion of gzip
        # and answers 406 for the dashboard's gzip-only embedded page (same
        # root cause cases_web.py's _http_get_raw already works around).
        # This was WEB-SEC-03's real bug: `curl http://<host>/` (gzip by
        # default) got 200 seconds after this client's bare identity request
        # got a non-200 for the same route.
        req = urllib.request.Request(url, method="GET", headers={"Accept-Encoding": "gzip"})
        if cookie:
            req.add_header("Cookie", f"kiln_sid={cookie}")
        try:
            with self._opener.open(req, timeout=self.timeout) as resp:
                return resp.getcode(), _decode_body(resp.read(), resp.headers), resp.headers
        except urllib.error.HTTPError as exc:
            try:
                raw = exc.read()
                if (exc.headers.get("Content-Encoding") or "").lower() == "gzip":
                    raw = gzip.decompress(raw)
                detail = raw.decode("utf-8", errors="replace")
            except (EOFError, zlib.error, OSError):
                detail = None
            except Exception:  # noqa: BLE001
                detail = None
            return exc.code, detail, exc.headers
        except (urllib.error.URLError, OSError) as exc:
            # OSError includes BodyDecodeError (bad gzip on a 2xx): a transport
            # problem -> (None, reason) so the judge reports INCONCLUSIVE.
            return None, str(exc), None

    def get_config(self) -> "Tuple[Optional[int], Optional[dict]]":
        # ROUTE_TIER_ADMIN. Goes through http_auth (auto-login on 401) --
        # unlike login()/get_session()/extend_session()/get_status() below,
        # this call is not itself testing what an unauthenticated or
        # explicit-cookie client sees; it needs to succeed whether the
        # board's web auth is already on or off when WEB-SEC-03 runs.
        status, text = _http_get_raw_authed(self.host, "/api/auth/config", timeout=self.timeout)
        return status, _parse_json(text)

    def set_web_password(self, username: str, password: str) -> "Tuple[Optional[int], Optional[dict]]":
        # ROUTE_TIER_ADMIN, same reasoning as get_config() above.
        status, text = _http_post_raw_authed(self.host, "/api/auth/security", {
            "cmd": "set_web_password", "role": "admin", "username": username, "password": password,
        }, timeout=self.timeout)
        return status, _parse_json(text)

    def set_lcd_pin(self, role: str, pin: str) -> "Tuple[Optional[int], Optional[dict]]":
        # ROUTE_TIER_ADMIN, same reasoning as get_config() above. Never
        # logs or persists `pin` -- the only caller (WEB-SEC-04) passes the
        # KILNCTL_LCD_PIN environment value, a real credential.
        status, text = _http_post_raw_authed(self.host, "/api/auth/security", {
            "cmd": "set_lcd_pin", "role": role, "pin": pin,
        }, timeout=self.timeout)
        return status, _parse_json(text)

    def set_policy(self, web_enabled: bool, lcd_enabled: bool, web_timeout_min: int,
                   lcd_timeout_min: int) -> "Tuple[Optional[int], Optional[dict]]":
        # ROUTE_TIER_ADMIN, same reasoning as get_config() above.
        status, text = _http_post_raw_authed(self.host, "/api/auth/security", {
            "cmd": "set_policy",
            "web_enabled": "1" if web_enabled else "0",
            "lcd_enabled": "1" if lcd_enabled else "0",
            "web_timeout_min": str(web_timeout_min),
            "lcd_timeout_min": str(lcd_timeout_min),
        }, timeout=self.timeout)
        return status, _parse_json(text)

    def login(self, username: str, password: str) -> "Tuple[Optional[int], Optional[str]]":
        status, _text, headers = _http_post_raw(self.host, "/api/auth/login", {
            "username": username, "password": password,
        })
        cookie = None
        self.last_login_set_cookie = None
        if headers is not None:
            for raw in (headers.get_all("Set-Cookie") or []) if hasattr(headers, "get_all") else []:
                name, _, rest = raw.partition("=")
                if name.strip() == "kiln_sid":
                    cookie = rest.split(";", 1)[0].strip()
                    self.last_login_set_cookie = _cookie_attr_flags(rest)
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

    def get_status_body(self, path: str, cookie: Optional[str] = None) -> "Tuple[Optional[int], Optional[str]]":
        """Like ``get_status`` but also returns the body, for WEB-SEC-03's
        dashboard probe -- so a FAIL there records recoverable evidence
        (status + a body snippet) instead of an empty ``observed`` dict."""
        status, text, _headers = self._get(path, cookie=cookie)
        return status, text

    def get_status_location(self, path: str, cookie: Optional[str] = None) -> "Tuple[Optional[int], Optional[str]]":
        """Like ``get_status`` but also returns the ``Location`` header, for
        WEB-SEC-03's ``/settings/zones`` probe -- a page shell must answer
        200 itself (lazy login, 2026-09-24), so a redirect is recorded with
        its target as evidence rather than followed."""
        status, _text, headers = self._get(path, cookie=cookie)
        location = None
        if headers is not None:
            location = headers.get("Location")
        return status, location


def _policy_from_config(cfg: dict) -> Dict[str, Any]:
    """Extract the four ``set_policy`` fields from a ``GET /api/auth/config``
    body. A timeout of ``-1`` is a valid, deliberate "never expire" value
    (``security_http_core.c:26-31`` returns it as-is) and must pass through
    unchanged -- only a missing/zero/negative-other-than--1 value falls back
    to a default of 30 minutes. Used by every case in this module that reads
    a policy snapshot before writing a test value, so a restore always
    writes back exactly what was read, `-1` included."""
    def _timeout(v: Any) -> int:
        if v is None:
            return 30
        if v == -1:
            return -1
        return v if v > 0 else 30

    return {
        "web_enabled": bool(cfg.get("web_enabled")),
        "lcd_enabled": bool(cfg.get("lcd_enabled")),
        "web_timeout_min": _timeout(cfg.get("web_timeout_min")),
        "lcd_timeout_min": _timeout(cfg.get("lcd_timeout_min")),
    }


def _sec_client(ctx: dict) -> Any:
    client = ctx.get("sec_client")
    if client is not None:
        return client
    host = ctx.get("host")
    if not host:
        from .. import mcp_server_ota  # local import: avoids importing kilnctrl.mcp_server at module load

        host = mcp_server_ota._ota_resolve_host(None)
    return _SecHttpClient(host)


def _probe_dashboard(client: Any) -> "Tuple[Optional[int], Optional[str]]":
    """Retries the unauthenticated dashboard GET up to
    ``_DASHBOARD_PROBE_ATTEMPTS`` times with ``_DASHBOARD_PROBE_BACKOFF_S``
    backoff (2026-09-24 fix, see the module-level comment above): a
    single-worker httpd can still be busy in the moment right after the
    ``set_policy(web_enabled=1)`` write that preceded this call.

    Falls back to ``client.get_status()`` (status only, no body) when a
    fake client (or an older real one) has no ``get_status_body`` -- keeps
    every pre-existing ``FakeSecClient``-based test passing unmodified."""
    get_body = getattr(client, "get_status_body", None)
    status: Optional[int] = None
    body: Optional[str] = None
    for attempt in range(_DASHBOARD_PROBE_ATTEMPTS):
        if get_body is not None:
            status, body = get_body("/")
        else:
            status, body = client.get_status("/"), None
        if status == 200:
            break
        if attempt < _DASHBOARD_PROBE_ATTEMPTS - 1:
            time.sleep(_DASHBOARD_PROBE_BACKOFF_S)
    return status, body


def _case_web_sec03(ctx: dict) -> CaseResult:
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
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
    if status0 is None or status0 == 401:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"GET /api/auth/config unreachable or unauthorised (status={status0}); no write attempted",
            observed={"status": status0},
        )
    if status0 != 200 or cfg0 is None:
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET /api/auth/config failed (status={status0})",
            observed={"status": status0},
        )
    if policy_unstored(cfg0):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="board shows no stored auth policy (false/false/-1/-1); a restore cannot put 'none' back, so nothing was written",
            observed={"policy_unstored": True},
        )

    # M2 (review 4 L3 pattern): never overwrite the admin password unless a
    # real login with the same credential already succeeds -- else the board
    # password silently changes to a stale environment value for good.
    pre_status, pre_cookie = client.login(username, password)
    if pre_status != 200 or not pre_cookie:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"harness credential is not the live one (login status={pre_status}); no password written",
            observed={"login_status": pre_status},
        )

    orig = _policy_from_config(cfg0)

    pw_ok = False
    enabled_ok = False
    dashboard_ok: Optional[bool] = None
    page_shell_open: Optional[bool] = None
    api_route_gated: Optional[bool] = None
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
                dashboard_status, dashboard_body = _probe_dashboard(client)
                dashboard_ok = dashboard_status == 200
                # Record the evidence regardless of verdict (never a
                # credential or Set-Cookie value -- this route is the
                # unauthenticated dashboard, and the snippet is capped well
                # short of anything auth-page-shaped) so a FAIL here is
                # reviewable instead of leaving `observed` with nothing to
                # show for it.
                state["dashboard_status"] = dashboard_status
                state["dashboard_body"] = (dashboard_body[:200] if dashboard_body else dashboard_body)
                # /settings/zones is a page shell (route_tier_table.h's
                # kPageShellUris[], lazy login, owner decision 2026-09-24:
                # opening the web UI never shows a login, no redirect, no
                # "authentication required" page) -- it must answer 200
                # itself with no session, never a redirect. The data-bearing
                # ADMIN route (GET /api/zones) is checked separately and must
                # still answer exactly 401: the shell being open is never
                # evidence the data behind it is.
                page_status, page_location = client.get_status_location("/settings/zones")
                page_shell_open = page_status == 200 and not page_location
                state["admin_page_status"] = page_status
                state["admin_page_location"] = page_location
                api_status = client.get_status("/api/zones")
                api_route_gated = api_status == 401
                state["api_route_status"] = api_status
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
        # Compare all four fields, not just web_enabled: this restore's own
        # set_policy call carries lcd_enabled/both timeouts too, and a
        # mismatch on any of them is just as much a hazard left on the
        # board as web_enabled itself would be.
        restore_matches = (
            readback_status == 200 and cfg_after is not None and
            _policy_from_config(cfg_after) == orig
        )
        state["restore"] = {
            "post_status": restore_status, "post_ok": restore_post_ok,
            "readback_status": readback_status, "readback_matches": restore_matches,
        }

    return J.judge_web_sec03(
        pw_ok=pw_ok, enabled_ok=enabled_ok, dashboard_ok=dashboard_ok,
        page_shell_open=page_shell_open, api_route_gated=api_route_gated,
        login_ok=login_ok, session_ok=session_ok,
        extend_ok=extend_ok, restore_ok=state["restore"]["post_ok"],
        restore_matches=state["restore"]["readback_matches"], state=state,
    )


# ---------------------------------------------------------------------------
# WEB-SEC-04 -- LCD PIN policy toggle round trip. Reads the admin LCD PIN
# from the KILNCTL_LCD_PIN environment variable (User scope, same convention
# as KILNCTL_WEB_USERNAME/PASSWORD) -- SKIPs, naming the variable, if it is
# unset, since there is no other sanctioned way to learn or choose a PIN
# that is safe to write to a shared bench board. `set_lcd_pin` is one-way
# hashed on the board with no read-back and no "clear just this PIN" route,
# so a fixed literal committed to this file would either collide with an
# operator's real PIN or, once written, could never be removed short of
# `clear_credentials` (which also wipes the web password) -- so this case
# never invents or hardcodes a PIN value:
#   - `admin_pin_set` already true on the board: nothing is written (an
#     unconditional overwrite risks losing access to a PIN nobody can read
#     back); this case trusts KILNCTL_LCD_PIN already matches whatever is
#     configured and only verifies the enable/readback round trip.
#   - `admin_pin_set` false: this case performs a Class C C16 credential
#     write, setting the board's admin LCD PIN from KILNCTL_LCD_PIN via
#     `cmd=set_lcd_pin`.
# Either way it then enables `lcd_enabled`, confirms the readback, and
# always restores the original four policy fields (never the PIN itself) in
# `finally`, hard-FAILing on any restore mismatch regardless of the rest.
# `set_lcd_pin` also ends every admin web session (`security_backend_web_auth.c:313-340`);
# `_SecHttpClient`'s calls all go through `http_auth`'s authenticated seam,
# which transparently re-logs in on the next request's 401, so this needs no
# special handling here. On a confirmed PASS, this hands the PIN pair to
# `ctx["_lcd_pin"]` so LCD-19 (depends_on WEB-SEC-04, registry.py) can drive
# the keypad via `UiTestClient.enter_pin()`. The wrong PIN is derived by
# flipping the right PIN's last digit, never a fixed literal like "0000",
# so it can never coincide with an operator-chosen right PIN by
# construction. The PIN value itself is never printed, logged, or persisted
# anywhere in `summary.json`/`transcript.md` -- only its presence, as
# `[bool]`.
# ---------------------------------------------------------------------------

_LCD_PIN_ENV = "KILNCTL_LCD_PIN"

#: security_http_core.h's SECURITY_HTTP_PIN_MIN/MAX -- security_pin_is_valid()
#: accepts 4-8 ASCII digits only.
_LCD_PIN_MIN_LEN = 4
_LCD_PIN_MAX_LEN = 8


def _lcd_pin_well_formed(pin: str) -> bool:
    """4-8 ASCII digits, matching the firmware's own PIN validator. Checked
    before the PIN is used for anything, so a malformed value can never
    reach int()/click_by_name()'s exception messages (both of which echo
    the offending character) or be written to the board."""
    return (
        isinstance(pin, str)
        and _LCD_PIN_MIN_LEN <= len(pin) <= _LCD_PIN_MAX_LEN
        and all(c in "0123456789" for c in pin)
    )


def _derive_wrong_lcd_pin(right_pin: str) -> str:
    """Flip the right PIN's last digit (mod 10) so the wrong PIN this case
    hands to LCD-19 can never equal the right one by construction -- unlike
    a fixed literal, which could coincide with whatever KILNCTL_LCD_PIN
    happens to hold."""
    last = right_pin[-1]
    flipped = str((int(last) + 1) % 10)
    return right_pin[:-1] + flipped


class LcdPinSeedError(Exception):
    """Raised by :func:`seed_lcd_pin` when the PIN cannot be sourced and (if
    needed) written. ``kind`` is ``"missing"`` when `KILNCTL_LCD_PIN` is
    simply unset -- never a hard failure, since a bench session may
    legitimately not want to touch the PIN -- or ``"fail"`` for anything
    else (a malformed value, a failed GET, or a `set_lcd_pin` that didn't
    confirm ``ok:true``). Never carries the PIN value itself. Each caller
    (`_case_web_sec04` below, and `cases_lcd._case_lcd19`) maps `kind` to its
    own verdict convention -- SKIP vs. NOT_RUN for "missing", FAIL either way
    for "fail" -- rather than this shared helper picking one for both."""

    def __init__(self, kind: str, reason: str, observed: Optional[dict] = None):
        super().__init__(reason)
        self.kind = kind
        self.reason = reason
        self.observed = observed or {}


def _resolve_lcd_pin(ctx: dict) -> Dict[str, Any]:
    """Source the board's admin LCD PIN from `KILNCTL_LCD_PIN` (or
    ``ctx["lcd_admin_pin"]``) and read its current config -- the read-only
    half of PIN seeding, shared by :func:`seed_lcd_pin` and
    `_case_web_sec04`. Raises `LcdPinSeedError` for a missing/malformed
    value or a failed GET (none of which ever touch the board or need a
    restore afterward); the exception's `.reason`/`.observed` are always
    safe to put straight into a `CaseResult` -- the PIN is never present in
    either. Returns ``{"client", "right_pin", "wrong_pin", "orig", "cfg0"}``
    on success.
    """
    right_pin = ctx.get("lcd_admin_pin") or os.environ.get(_LCD_PIN_ENV)
    if not right_pin:
        # Checked before _sec_client(ctx) is ever called -- with no
        # ctx["sec_client"]/ctx["host"] override, that call falls through to
        # resolving a real board host, which a caller with no PIN configured
        # (e.g. a test, or a bench session that never intends to touch the
        # PIN) must never trigger just to learn that.
        raise LcdPinSeedError("missing", f"{_LCD_PIN_ENV} not set in the environment")
    if not _lcd_pin_well_formed(right_pin):
        # Never echo the value (or its length) -- name the variable only.
        raise LcdPinSeedError(
            "fail",
            f"{_LCD_PIN_ENV} is set but is not {_LCD_PIN_MIN_LEN}-{_LCD_PIN_MAX_LEN} ASCII digits",
        )
    wrong_pin = _derive_wrong_lcd_pin(right_pin)

    client = _sec_client(ctx)
    status0, cfg0 = client.get_config()
    if status0 != 200 or cfg0 is None:
        raise LcdPinSeedError(
            "fail", f"GET /api/auth/config failed (status={status0})", {"status": status0}
        )

    orig = _policy_from_config(cfg0)
    return {"client": client, "right_pin": right_pin, "wrong_pin": wrong_pin, "orig": orig, "cfg0": cfg0}


def _write_lcd_pin_if_needed(client: Any, cfg0: dict, right_pin: str) -> "Tuple[bool, Dict[str, Any]]":
    """Write `right_pin` via ``cmd=set_lcd_pin`` unless the board already has
    an admin PIN set (module comment above `_LCD_PIN_ENV`: never overwrite
    one). Never raises -- returns ``(pin_set_ok, state)`` so a caller with
    its own try/finally (namely `_case_web_sec04`, which must still attempt
    its policy restore even when this write fails) can decide what to do
    next itself."""
    admin_pin_already_set = bool(cfg0.get("admin_pin_set"))
    state: Dict[str, Any] = {"admin_pin_set_before": admin_pin_already_set}
    if admin_pin_already_set:
        state["set_lcd_pin_skipped"] = "admin_pin_set was already true"
        return True, state
    pin_status, pin_resp = client.set_lcd_pin("admin", right_pin)
    pin_set_ok = pin_status == 200 and bool(pin_resp) and pin_resp.get("ok") is True
    state["set_lcd_pin_status"] = pin_status
    return pin_set_ok, state


def seed_lcd_pin(ctx: dict) -> Dict[str, Any]:
    """Source, and if needed write, the board's admin LCD PIN -- the ONE
    sanctioned path that ever calls ``cmd=set_lcd_pin`` (module comment
    above `_LCD_PIN_ENV`). This is WEB-SEC-04's own seeding logic
    (`_resolve_lcd_pin` + `_write_lcd_pin_if_needed`), composed here so
    LCD-19 (`cases_lcd._case_lcd19`) can reuse the exact same path for a
    standalone LCD-suite run that never executed WEB-SEC-04 (a different
    suite) in the same session, rather than copying or re-deriving it.
    LCD-19 has no policy state of its own to restore on a seeding failure
    (unlike WEB-SEC-04), so this raises outright rather than returning a
    partial result the way `_write_lcd_pin_if_needed` does for
    `_case_web_sec04`.

    Returns ``{"right_pin", "wrong_pin", "orig", "state"}`` on success --
    ``orig`` is the pre-existing policy snapshot (`_policy_from_config`),
    ``state`` a partial observed-state dict (admin_pin_set_before / whether
    a write happened) with no PIN value in it. Raises `LcdPinSeedError`
    otherwise; the exception's `.reason`/`.observed` are always safe to put
    straight into a `CaseResult` -- the PIN is never present in either.
    """
    resolved = _resolve_lcd_pin(ctx)
    if resolved["cfg0"].get("admin_pin_set"):
        # The config GET only says a PIN is set, and no route verifies one
        # without side effects. Do not overwrite it and do not trust it
        # here: return it flagged unverified so the caller proves it with a
        # real keypad unlock (LCD-19: enter_pin, INCONCLUSIVE if it fails).
        return {
            "right_pin": resolved["right_pin"], "wrong_pin": resolved["wrong_pin"],
            "orig": resolved["orig"], "unverified": True,
            "state": {"pin_unverified": True, "admin_pin_set_before": True},
        }
    pin_set_ok, state = _write_lcd_pin_if_needed(resolved["client"], resolved["cfg0"], resolved["right_pin"])
    if not pin_set_ok:
        raise LcdPinSeedError(
            "fail", f"set_lcd_pin did not confirm ok:true (status={state.get('set_lcd_pin_status')})", state
        )
    return {
        "right_pin": resolved["right_pin"], "wrong_pin": resolved["wrong_pin"],
        "orig": resolved["orig"], "state": state,
    }


def _case_web_sec04(ctx: dict) -> CaseResult:
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
    try:
        resolved = _resolve_lcd_pin(ctx)
    except LcdPinSeedError as exc:
        verdict = Verdict.SKIP if exc.kind == "missing" else Verdict.FAIL
        return CaseResult(verdict, reason=exc.reason, observed=exc.observed)

    client = resolved["client"]
    right_pin = resolved["right_pin"]
    wrong_pin = resolved["wrong_pin"]
    orig = resolved["orig"]
    if policy_unstored(resolved["cfg0"]):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="board shows no stored auth policy (false/false/-1/-1); a restore cannot put 'none' back, so nothing was written",
            observed={"policy_unstored": True},
        )

    pin_set_ok, seed_state = _write_lcd_pin_if_needed(client, resolved["cfg0"], right_pin)
    enabled_ok = False
    readback_enabled: Optional[bool] = None
    state: Dict[str, Any] = {"orig": orig}
    state.update(seed_state)

    try:
        if pin_set_ok:
            en_status, en_resp = client.set_policy(
                orig["web_enabled"], True, orig["web_timeout_min"], orig["lcd_timeout_min"])
            enabled_ok = en_status == 200 and bool(en_resp) and en_resp.get("ok") is True
            state["set_policy_enable_status"] = en_status
            if enabled_ok:
                rb_status, cfg1 = client.get_config()
                readback_enabled = rb_status == 200 and cfg1 is not None and bool(cfg1.get("lcd_enabled")) is True
                state["readback_status"] = rb_status
    finally:
        # Unconditional, even on an exception above -- only the policy
        # fields are restorable here (see module comment); the PIN this
        # case may have set stays on the board.
        restore_status, restore_resp = client.set_policy(
            orig["web_enabled"], orig["lcd_enabled"], orig["web_timeout_min"], orig["lcd_timeout_min"])
        restore_post_ok = restore_status == 200 and bool(restore_resp) and restore_resp.get("ok") is True
        readback_status, cfg_after = client.get_config()
        restore_matches = (
            readback_status == 200 and cfg_after is not None and
            _policy_from_config(cfg_after) == orig
        )
        state["restore"] = {
            "post_status": restore_status, "post_ok": restore_post_ok,
            "readback_status": readback_status, "readback_matches": restore_matches,
        }

    result = J.judge_web_sec04(
        pin_set_ok=pin_set_ok, enabled_ok=enabled_ok, readback_enabled=readback_enabled,
        restore_ok=state["restore"]["post_ok"], restore_matches=state["restore"]["readback_matches"],
        state=state,
    )
    if result.verdict == Verdict.PASS and seed_state.get("admin_pin_set_before"):
        # A pre-existing PIN is not proven to be the env PIN (the config GET
        # only says one is set). Hand it to LCD-19 flagged unverified; LCD-19
        # proves it by a real keypad unlock and is INCONCLUSIVE otherwise.
        result.observed = dict(result.observed or {}, pin_unverified=True)
        ctx["_lcd_pin"] = {"right_pin": right_pin, "wrong_pin": wrong_pin, "unverified": True}
    elif result.verdict == Verdict.PASS:
        # LCD-19 (registry.py depends_on WEB-SEC-04) reads this to drive
        # UiTestClient.enter_pin() -- never populated on anything less than
        # a confirmed PASS here, so LCD-19 never acts on a PIN that might
        # not actually be live on the board.
        ctx["_lcd_pin"] = {"right_pin": right_pin, "wrong_pin": wrong_pin}
    return result


def _case_web_zone14(ctx: dict) -> CaseResult:
    """WEB-ZONE-14: read-only. GET /api/zones (ADMIN tier, authed seam) and
    judge the zone-graphic inputs well-formed and self-consistent. A second
    GET must report the same ``generation`` or the config moved mid-read and
    the verdict would describe two different configs, so that is
    INCONCLUSIVE."""
    status, body = _get_json(ctx, "/api/zones")
    result = J.judge_web_zone_graphic(status, body)
    if result.verdict == Verdict.PASS:
        status2, body2 = _get_json(ctx, "/api/zones")
        if status2 != 200 or not isinstance(body2, dict):
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=f"second read failed (status={status2}); stability not confirmed",
                observed=result.observed,
            )
        g1 = body.get("generation") if isinstance(body, dict) else None
        g2 = body2.get("generation")
        if g1 is None and g2 is None:
            result.observed["generation"] = "not reported"
        elif g1 != g2:
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=f"zones config generation moved between reads ({g1!r} -> {g2!r}); not judged",
                observed=result.observed,
            )
    return result


#: Wire this wave's judge functions into the shared REGISTRY (same
#: convention as cases_web.py's own tail).
_CASE_FUNCS = {
    "WEB-DASH-13": cfg_guarded(_case_dash13),
    "WEB-DIAG-07": _case_diag07,
    "WEB-DIAG-08": cfg_guarded(_case_diag08),
    "WEB-ZONE-14": _case_web_zone14,
    "WEB-SEC-04": _case_web_sec04,
    "WEB-SEC-03": _case_web_sec03,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
