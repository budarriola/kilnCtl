"""Wave 1a: the WEB suite's `-01` render cases plus the generated route-tier
sweep (WEB-X-01/WEB-X-03).

Same shape as cases_smoke.py: each `_case_XXX(ctx)` fetches, then hands the
result to a pure judge function in judgments.py. `ctx["host"]` is the board
address; `ctx["web_client"]` is an optional pre-built ``WebUiClient`` (unit
tests inject a fake one so no real HTTP happens -- see cases_smoke.py's
docstring for the same convention with ``ctx["srv"]``).

**Scope, per the task this wave was cut from (plan doc §8 Wave 1a):**
read-only only. The 17 page-render cases and WEB-X-01 (`nav.js`'s menu
shape) only ever issue GET requests. WEB-X-03, the per-route auth-tier
sweep, is *generated at run time* by parsing
``firmware/KilnFW/App/drivers/http/route_tier_table.h``'s ``ROUTE_TIER(...)``
rows (never hardcoded, so it cannot go stale as routes are added/removed --
`check_route_tier_coverage.ps1` already guarantees every real route has a
row there) -- but it only ever *exercises* the GET rows. A POST/PUT/DELETE
row would have to be invoked to observe its tier behaviour, and invoking one
unauthenticated is itself a write this wave must not perform (task
instruction: "Read-only cases only for this wave"); those rows are still
parsed and reported (so the row count and tier counts are visible and can be
cross-checked against `check_uri_handler_cap.ps1`'s 150), just never given
an HTTP call of their own. A later wave that owns web-auth writes
(WEB-SEC-03, Wave 2 per the plan) is the natural place to extend the sweep
to non-GET rows under a real session.
"""
from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, List, Optional, Tuple

from . import judgments as J
from . import operator as OP
from .registry import CaseResult, Verdict, get_case


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/bench_test/ -> repo root is five levels
    up. Same convention as cases_smoke.py/report.default_logs_root()."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", ".."))


def _web_client(ctx: dict):
    client = ctx.get("web_client")
    if client is not None:
        return client
    from .web_ui_client import WebUiClient
    from .. import mcp_server_ota  # local import: avoids importing kilnctrl.mcp_server at module load

    host = ctx.get("host") or mcp_server_ota._ota_resolve_host(None)
    return WebUiClient(f"http://{host}")


def _http_get_raw(host: str, path: str, timeout: float = 5.0) -> "tuple[Optional[int], Optional[str]]":
    """Bare, no-cookie GET -- used for the tier sweep, which must observe
    the response an unauthenticated client actually gets, not one built
    through http_auth's credential-injecting urlopen()."""
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        return exc.code, None
    except (urllib.error.URLError, OSError) as exc:
        return None, str(exc)


# ---------------------------------------------------------------------------
# The 17 page-render cases (WEB-*-01). (path, landmark element id, whether
# nav.js is expected) -- hand-verified against the *_page.html files under
# firmware/KilnFW/App/drivers/http/ and .../net/{ota,security,
# wifi_provision}_page.html and their httpd_uri_t registrations (plan §9
# sources). `/login` is the one page with no shared nav chrome (it loads
# before a session exists).
# ---------------------------------------------------------------------------
_PAGES: List[Tuple[str, str, str, bool]] = [
    ("WEB-DASH-01", "/", "runBtn", True),
    ("WEB-PROF-01", "/profiles", "favList", True),
    ("WEB-ZONE-01", "/settings/zones", "themeBtn", True),
    ("WEB-SAF-01", "/safety", "safetyReady", True),
    ("WEB-STIM-01", "/settings/safety", "themeBtn", True),
    ("WEB-COMM-01", "/safety/commissioning", "linkDownBanner", True),
    ("WEB-RDY-01", "/readiness", "items", True),
    ("WEB-WIZ-01", "/setup", "gstepper", True),
    ("WEB-DIAG-01", "/diagnostics", "staleBanner", True),
    ("WEB-OTA-01", "/ota", "interlockBox", True),
    ("WEB-WIFI-01", "/wifi", "statusRows", True),
    ("WEB-SEC-01", "/settings/security", "kcSecAdminUser", True),
    ("WEB-BAK-01", "/settings/backup", "restoreBtn", True),
    ("WEB-KCFG-01", "/settings/kiln_configs", "kilnConfigCard", True),
    ("WEB-SET-01", "/settings", "swResetBtn", True),
    ("WEB-DISP-01", "/settings/display", "kcDpBrightness", True),
    ("WEB-LOG-01", "/login", "username", False),
]


def _make_render_case(path: str, landmark_id: str, expect_nav: bool):
    def _case(ctx: dict) -> CaseResult:
        client = _web_client(ctx)
        try:
            html = client.goto(path)
        except Exception as exc:  # noqa: BLE001 - report as a FAIL, not a crash
            return J.judge_web_render(None, landmark_id, expect_nav, error=f"GET {path} failed: {exc}")
        return J.judge_web_render(html, landmark_id, expect_nav)

    return _case


# ---------------------------------------------------------------------------
# WEB-X-01 -- nav.js's menu shape (15 links, one expanding group).
# ---------------------------------------------------------------------------

def _case_web_x01(ctx: dict) -> CaseResult:
    host = ctx.get("host")
    status, text = _http_get_raw(host, "/nav.js") if host else (None, None)
    if status != 200 or text is None:
        return J.judge_nav_menu(None, None)
    href_count = len(re.findall(r"href\s*:\s*'[^']*'", text))
    has_group_expand = ("activeFor" in text) and ("children" in text)
    return J.judge_nav_menu(text, href_count, has_group_expand=has_group_expand)


# ---------------------------------------------------------------------------
# WEB-X-03 -- the generated per-route auth-tier sweep.
# ---------------------------------------------------------------------------

#: Tiers that must be reachable with no credential regardless of session
#: state (route_tier_table.h's own doc comment: OPEN "safe to reveal/costs
#: nothing", SAFETY_REDUCE "can only make the kiln safer").
_ALWAYS_OPEN_TIERS = frozenset({"ROUTE_TIER_OPEN", "ROUTE_TIER_SAFETY_REDUCE", "ROUTE_TIER_ADMIN_BOOTSTRAP"})
_ADMIN_TIERS = frozenset({"ROUTE_TIER_ADMIN", "ROUTE_TIER_USER"})

#: GET routes that are read-only by tier but have a real side effect on the
#: board, so the sweep must never invoke them even though their tier would
#: otherwise mark them safe to exercise. Found by grepping
#: route_tier_table.h's GET rows against the *_http.c handlers for anything
#: named scan/reset/clear/start/challenge/reboot/erase, then reading each
#: hit's handler body (wifi_provision_http.c's scan_get_handler,
#: networks_get_handler, ota_http.c's ota_challenge_get_handler):
#:   /scan                 -- calls wifi_prov_scan(), which can disturb the
#:                            Wi-Fi link this suite's own HTTP calls run over.
#:   /networks             -- also calls wifi_prov_scan() internally (a
#:                            merged saved+scanned status readout), same
#:                            disturbance as /scan even though the name
#:                            doesn't say so.
#:   /api/ota/challenge    -- mints/rotates the OTA auth nonce
#:                            (ota_auth_nonce_issue()) on every call.
#: Excluded rows are recorded in the case's detail as "excluded: side
#: effect" and never fetched -- see the loop below.
_SIDE_EFFECT_EXCLUDE = frozenset({"/scan", "/networks", "/api/ota/challenge"})

_ROUTE_TIER_ROW_RE = re.compile(
    r'ROUTE_TIER\(\s*"([^"]*)"\s*,\s*(HTTP_[A-Za-z0-9_]+)\s*,\s*(ROUTE_TIER_[A-Za-z0-9_]+)\s*\)'
)


def parse_route_tier_table(text: str) -> List[Tuple[str, str, str]]:
    """Parse every ``ROUTE_TIER("uri", HTTP_METHOD, ROUTE_TIER_X)`` row out
    of route_tier_table.h's source text. Same regex shape as
    ``tools/check_route_tier_coverage.ps1``'s ``$rowPattern`` -- kept as a
    literal copy (not imported: one is PowerShell, one is Python) rather
    than each guessing the other's syntax; if that script's pattern ever
    changes, this one needs the matching update, the same "two copies of
    one fact" caveat route_tier_table.h's own header names for the reverse
    direction.

    Returns ``(uri, http_method, tier)`` tuples in source order. Never
    hardcodes a route: fed the real file's text, this call is the whole
    sweep's source of truth."""
    return [(uri, method, tier) for uri, method, tier in _ROUTE_TIER_ROW_RE.findall(text)]


def _route_tier_table_path(ctx: dict) -> str:
    return ctx.get("route_tier_table_path") or os.path.join(
        ctx.get("repo_root") or _repo_root(),
        "firmware", "KilnFW", "App", "drivers", "http", "route_tier_table.h",
    )


def _case_web_x03(ctx: dict) -> CaseResult:
    path = _route_tier_table_path(ctx)
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as exc:
        return CaseResult(Verdict.FAIL, reason=f"could not read {path}: {exc}", observed={})
    rows = parse_route_tier_table(text)
    if not rows:
        return CaseResult(Verdict.FAIL, reason=f"no ROUTE_TIER(...) rows parsed from {path}", observed={})

    host = ctx.get("host")
    web_enabled: Optional[bool] = None
    if host:
        status, body = _http_get_raw(host, "/api/auth/config")
        if status == 200 and body:
            web_enabled = '"web_enabled":true' in body.replace(" ", "")

    results: List[dict] = []
    for uri, method, tier in rows:
        row: dict = {"uri": uri, "method": method, "tier": tier, "exercised": False, "ok": True}
        if uri in _SIDE_EFFECT_EXCLUDE:
            # Read-only by tier but has a real side effect -- see
            # _SIDE_EFFECT_EXCLUDE's comment. Never fetched.
            row["detail"] = "excluded: side effect"
            results.append(row)
            continue
        if method != "HTTP_GET" or not host:
            # Non-GET rows (or no board attached, e.g. a unit test feeding
            # ctx with no "host") are recorded, never invoked -- see this
            # module's docstring.
            results.append(row)
            continue
        status, _body = _http_get_raw(host, uri)
        row["exercised"] = True
        row["status"] = status
        if tier in _ALWAYS_OPEN_TIERS:
            row["ok"] = status is not None and status < 400
        elif tier in _ADMIN_TIERS:
            if web_enabled:
                row["ok"] = status in (401, 403) or (status is not None and 300 <= status < 400)
            else:
                # Auth is off on this bench (owner decision, plan §7.7/7.8
                # threading web credentials in is a later wave) -- an ADMIN
                # route answering normally is expected, not a violation.
                row["ok"] = True
        else:
            row["ok"] = True
        results.append(row)

    return J.judge_route_tier_sweep(results)


def _wifi_status(host: str) -> "tuple[Optional[int], Optional[dict]]":
    """GET /status (wifi_provision_http.c's status_get_handler, ROUTE_TIER_OPEN)
    -- {"mode": "home"|"ap", ...}. Bare, unauthenticated GET like the tier
    sweep above: this route is OPEN, and Wi-Fi credentials are never read
    from or logged by this case (plan §6 rule 9) -- only the `mode` field is
    ever inspected."""
    status, text = _http_get_raw(host, "/status")
    if status != 200 or not text:
        return status, None
    try:
        body = json.loads(text)
    except (ValueError, TypeError):
        return status, None
    return status, body if isinstance(body, dict) else None


def _case_web_wifi06(ctx: dict) -> CaseResult:
    """WEB-WIFI-06: AP-mode toggle is operator-only (plan §5, WEB-WIFI
    section) -- an unattended run SKIPs (never FAILs, never hangs). When
    attended, the operator is asked to trigger AP mode (e.g. via the Wi-Fi
    page's own control, or by disconnecting the board's home network) and
    confirm they saw it come up; this case also reads the board's own
    `/status` before asking (in case AP mode is already up when the case
    starts) and afterward to confirm the round trip back to `mode=home` --
    see judgments.judge_wifi_mode_returned_home for why neither signal alone
    is trusted."""
    skip = OP.require_attended(ctx)
    if skip is not None:
        return skip

    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.NOT_RUN, reason="no host configured", observed={})

    mode_during_ap: Optional[str] = None
    _status, body = _wifi_status(host)
    if body:
        mode_during_ap = body.get("mode")

    confirmed = OP.ask_operator(
        ctx,
        "Toggle the board's Wi-Fi to AP mode, confirm you see the AP network appear, "
        "then toggle it back to normal (home) mode. Did you see the AP network?",
        timeout_s=180.0,
    )

    _status2, body2 = _wifi_status(host)
    mode_after = body2.get("mode") if body2 else None

    return J.judge_wifi_mode_returned_home(mode_during_ap, mode_after, confirmed)


#: Deliberately never the real password -- WEB-SEC-05 exists to prove every
#: attempt fails and the endpoint starts refusing outright (429), so this is
#: a fixed, obviously-wrong string, never anything read from the environment.
_SEC05_WRONG_PASSWORD = "bench-test-deliberately-wrong"
_SEC05_ATTEMPTS = 6


def _sec05_login_attempt(host: str, username: str, password: str, timeout: float = 5.0) -> Optional[int]:
    url = f"http://{host}/api/auth/login"
    body = urllib.parse.urlencode({"username": username, "password": password}).encode("ascii")
    req = urllib.request.Request(
        url, data=body, method="POST", headers={"Content-Type": "application/x-www-form-urlencoded"}
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode()
    except urllib.error.HTTPError as exc:
        return exc.code
    except (urllib.error.URLError, OSError):
        return None


def _case_web_sec05(ctx: dict) -> CaseResult:
    """WEB-SEC-05: lockout (plan §5.1 WEB-SEC section). Credentials come
    ONLY from KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD (never persisted or
    logged) -- but only the USERNAME is ever actually sent; the password is
    always the fixed wrong string above, since this case's entire point is
    to force failed attempts. SKIPs (not FAIL) when no host or no username
    is configured, since a username is needed to exercise the real login
    path rather than an arbitrary/nonexistent one.

    No policy this case touches is reversible in a `finally`: the lockout it
    trips is a genuine, self-expiring per-IP counter in the board's own RAM
    (web_auth_login_http.c's login_lockout_slot_t table), not a toggle this
    tooling flips -- there is no admin "clear my own lockout" route, by
    design (memory project_login_lockout_saturation_accepted, an accepted
    owner decision, not a defect to work around). This is exactly why the
    case is pinned dead last in the suite (registry._ALWAYS_LAST): nothing
    scheduled after it in the same run can be blocked by its own lockout."""
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.NOT_RUN, reason="no host configured", observed={})
    username = os.environ.get("KILNCTL_WEB_USERNAME")
    if not username:
        return CaseResult(Verdict.SKIP, reason="KILNCTL_WEB_USERNAME not set in the environment")

    status_codes = [
        _sec05_login_attempt(host, username, _SEC05_WRONG_PASSWORD) for _ in range(_SEC05_ATTEMPTS)
    ]
    return J.judge_login_lockout(status_codes)


#: Wire this wave's judge functions into the shared REGISTRY (see
#: registry.py's module docstring for why ids are declared there and wired
#: up here at import time).
_CASE_FUNCS = {cid: _make_render_case(path, landmark_id, expect_nav) for cid, path, landmark_id, expect_nav in _PAGES}
_CASE_FUNCS["WEB-X-01"] = _case_web_x01
_CASE_FUNCS["WEB-X-03"] = _case_web_x03
_CASE_FUNCS["WEB-WIFI-06"] = _case_web_wifi06
_CASE_FUNCS["WEB-SEC-05"] = _case_web_sec05

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
