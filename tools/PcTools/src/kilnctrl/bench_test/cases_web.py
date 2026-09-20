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

import os
import re
import urllib.error
import urllib.request
from typing import Any, List, Optional, Tuple

from . import judgments as J
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


#: Wire this wave's judge functions into the shared REGISTRY (see
#: registry.py's module docstring for why ids are declared there and wired
#: up here at import time).
_CASE_FUNCS = {cid: _make_render_case(path, landmark_id, expect_nav) for cid, path, landmark_id, expect_nav in _PAGES}
_CASE_FUNCS["WEB-X-01"] = _case_web_x01
_CASE_FUNCS["WEB-X-03"] = _case_web_x03

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
