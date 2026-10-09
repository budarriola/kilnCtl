"""WEB-WIFI-02..05, WEB-SEC-02/06, WEB-BAK-02..04, WEB-KCFG-02..05,
WEB-LOG-02/03 and WEB-X-02 judges (docs/BENCH_TEST_WEB_JUDGES_PLAN.md 4.7).

Hard limits honoured here: nothing POSTs a Wi-Fi provisioning writer, a
credential clear, backup import, or kiln-config apply. NEEDS-OWNER ids take the
doc's recommended reduction (owner 2026-10-09). No ERROR verdict.
"""
from __future__ import annotations

import ast
import hashlib
import json
import re
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .cases_web import _web_client
from .cases_web_diag import _html, _missing, mutating_gate
from .cases_web_rw import (_WIFI_WRITE_DENYLIST, ForbiddenWrite, _get_json, _get_text, _policy_from_config,
                           _post_json, _post_json_body, _post_raw, _sec_client)
from .registry import CaseResult, Verdict, get_case

_R = CaseResult


def _unwrap_items(body: Any) -> Optional[list]:
    if isinstance(body, list):
        return body
    if isinstance(body, dict) and isinstance(body.get("items"), list):
        return body["items"]
    return None


# ---------------------------------------------------------------------------
# WEB-WIFI-02..05
# ---------------------------------------------------------------------------

def _case_wifi02(ctx: dict) -> CaseResult:
    st, s = _get_json(ctx, "/status")
    if st != 200 or not isinstance(s, dict):
        return _R(Verdict.INCONCLUSIVE, reason=f"GET /status unreadable (status={st})")
    ssid = s.get("ssid")
    if s.get("mode") != "home" or s.get("sta_connected") is not True or not ssid:
        return _R(Verdict.INCONCLUSIVE, reason="not in home mode / station not connected / ssid empty",
                  observed={"mode": s.get("mode"), "sta_connected": s.get("sta_connected")})
    st, nets = _get_json(ctx, "/networks")
    if st != 200 or not isinstance(nets, list):
        return _R(Verdict.FAIL, reason=f"GET /networks not 200 with a list (status={st})", observed={"status": st})
    st2, s2 = _get_json(ctx, "/status")
    if isinstance(s2, dict) and s2.get("sta_connected") is not True:
        return _R(Verdict.INCONCLUSIVE, reason="station dropped during the scan (observation, not a FAIL)",
                  observed={"sta_connected_after": s2.get("sta_connected")})
    match = [n for n in nets if isinstance(n, dict) and n.get("ssid") == ssid]
    if len(match) != 1:
        return _R(Verdict.FAIL, reason=f"bench SSID appears {len(match)} times in /networks, expected 1")
    n = match[0]
    bad = [k for k in ("saved", "in_range", "connected") if n.get(k) is not True]
    if bad:
        return _R(Verdict.FAIL, reason=f"bench SSID entry has {bad} not true", observed={"entry_keys": sorted(n)})
    if isinstance(s2, dict) and s2.get("ssid") != ssid:
        return _R(Verdict.FAIL, reason="ssid changed across the scan")
    return _R(Verdict.PASS)


_WIFI03_MARKERS = ('id="apSection"', "#apSection { display: none; }", 'id="apQrCanvas"',
                   "var KilnQr", "function renderApQr(")


def _case_wifi03(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/wifi")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    st, s = _get_json(ctx, "/status")
    if st != 200 or not isinstance(s, dict):
        return _R(Verdict.INCONCLUSIVE, reason=f"GET /status unreadable (status={st})")
    miss = list(_missing(html, _WIFI03_MARKERS))
    # The call sits inside a braced if-block; match it whitespace-tolerantly.
    if not re.search(r"if\s*\(\s*s\.mode\s*===\s*'ap'\s*\)\s*\{?\s*renderApQr\s*\(", html):
        miss.append("if (s.mode === 'ap') renderApQr(")
    if miss:
        return _R(Verdict.FAIL, reason=f"/wifi HTML missing {miss}", observed={"missing": miss})
    if s.get("mode") == "ap":
        return _R(Verdict.INCONCLUSIVE, reason="mode is ap (operator territory, WEB-WIFI-06)")
    if s.get("mode") != "home":
        return _R(Verdict.INCONCLUSIVE, reason=f"mode {s.get('mode')!r} is not home")
    return _R(Verdict.PASS)


def _case_wifi04(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/wifi")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    st, s = _get_json(ctx, "/status")
    if st != 200 or not isinstance(s, dict):
        return _R(Verdict.INCONCLUSIVE, reason=f"GET /status unreadable (status={st})")
    problems = [f"missing {m}" for m in _missing(
        html, ("ipModeDhcpBtn", "ipModeStaticBtn", "staticIpFields", "applyIpModeUi"))]
    if not re.search(r'id="staticIpFields"[^>]*style="display:\s*none', html):
        problems.append("staticIpFields is not display:none by default")
    m = re.search(r"ipModeStaticBtn['\"]\)?\.(?:addEventListener\(\s*['\"]click['\"]\s*,|onclick\s*=)(.{0,600})",
                  html, re.S)
    if not m:
        problems.append("Static click handler not found")
    else:
        body = m.group(1).split("});", 1)[0]
        if "fetch(" in body or "submitIpConfig(" in body:
            problems.append("Static handler submits (fetch/submitIpConfig)")
    if s.get("ip_mode") not in ("dhcp", "static"):
        problems.append(f"ip_mode {s.get('ip_mode')!r} not in dhcp/static")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed={"problems": problems})
    return _R(Verdict.PASS, observed={"ip_mode": s["ip_mode"]})


_WIFI_FORBIDDEN_LITERALS = tuple(_WIFI_WRITE_DENYLIST["path_suffixes"])
_WIFI_FORBIDDEN_IMPORTS = ("wifi_uart", "wifi_prov_http_client", "gui_wifi_firing",
                           "mcp_server_wifi", "mcp_server_network")
_DENYLIST_NAME = "_WIFI_WRITE_DENYLIST"


def scan_bench_sources(directory: Path, forbidden_literals=_WIFI_FORBIDDEN_LITERALS,
                       forbidden_imports=_WIFI_FORBIDDEN_IMPORTS,
                       forbidden_extra: Tuple[str, ...] = ()) -> List[str]:
    """AST scan: string literals (docstrings and the deny-list constant exempt)
    and imports. Returns human-readable hits."""
    hits: List[str] = []
    for path in sorted(directory.glob("*.py")):
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"))
        except (OSError, SyntaxError) as exc:
            hits.append(f"{path.name}: unparsable ({exc})")
            continue
        exempt = set()
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                b = node.body
                if b and isinstance(b[0], ast.Expr) and isinstance(getattr(b[0], "value", None), ast.Constant):
                    exempt.add(id(b[0].value))
            if isinstance(node, (ast.Assign, ast.AnnAssign)):
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                if any(isinstance(t, ast.Name) and t.id == _DENYLIST_NAME for t in targets):
                    for sub in ast.walk(node):
                        exempt.add(id(sub))
        needles = tuple(forbidden_literals) + tuple(forbidden_extra)
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in exempt:
                for needle in needles:
                    if needle in node.value:
                        hits.append(f"{path.name}:{node.lineno}: literal containing {needle!r}")
            elif isinstance(node, ast.Import):
                for a in node.names:
                    if a.name.split(".")[-1] in forbidden_imports:
                        hits.append(f"{path.name}:{node.lineno}: import {a.name}")
            elif isinstance(node, ast.ImportFrom):
                parts = (node.module or "").split(".") + [a.name for a in node.names]
                if any(p in forbidden_imports for p in parts):
                    hits.append(f"{path.name}:{node.lineno}: import from {node.module}")
    return hits


def _bench_dir(ctx: dict) -> Path:
    return Path(ctx.get("bench_test_dir") or Path(__file__).parent)


def _guard_enforced(ctx: dict) -> Optional[str]:
    """None when every POST helper raises ForbiddenWrite on a denied path."""
    prov, forget, ipc = _WIFI_WRITE_DENYLIST["path_suffixes"]
    probes = (
        ("_post_json", lambda: _post_json(ctx, forget, {})),
        ("_post_json", lambda: _post_json(ctx, prov, {"ssid": "x"})),
        ("_post_json", lambda: _post_json(ctx, ipc, {})),
        ("_post_raw", lambda: _post_raw(ctx, forget, {})),
        ("_post_json_body", lambda: _post_json_body(ctx, prov, {})),
    )
    for name, fn in probes:
        try:
            fn()
        except ForbiddenWrite:
            continue
        except Exception as exc:  # noqa: BLE001
            return f"{name} raised {type(exc).__name__}, not ForbiddenWrite"
        return f"{name} did not raise on a denied path"
    return None


def _wifi_tuple(ctx: dict) -> Optional[tuple]:
    st, s = _get_json(ctx, "/status")
    if st != 200 or not isinstance(s, dict):
        return None
    return tuple(s.get(k) for k in ("mode", "state", "ssid", "ip_mode"))


def _sec06_admin_probe(ctx: dict) -> Optional[bool]:
    """Run-start baseline for WEB-SEC-06 (read-only GET /api/auth/config)."""
    st, cfg = _get_json(ctx, "/api/auth/config")
    if st != 200 or not isinstance(cfg, dict) or "admin_password_set" not in cfg:
        return None
    return cfg["admin_password_set"]


def _case_wifi05(ctx: dict) -> CaseResult:
    # The guard probes run with the real (or faked) transport seams; a denied
    # path raises before any transport call, so nothing is ever sent.
    hits = scan_bench_sources(_bench_dir(ctx))
    guard = _guard_enforced(ctx)
    problems = list(hits)
    if guard:
        problems.append(f"guard: {guard}")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems[:6]), observed={"hits": len(problems)})
    w06 = (ctx.get("_results") or {}).get("WEB-WIFI-06")
    if w06 is not None and w06.verdict != Verdict.NOT_RUN:
        return _R(Verdict.INCONCLUSIVE, reason="WEB-WIFI-06 ran in this run; judged (a) and (b) only: both clean")
    start = ctx.get("_wifi_status_start")
    now = _wifi_tuple(ctx)
    if start is None:
        ctx["_wifi_status_start"] = now
        return _R(Verdict.PASS, observed={"c_baseline": "taken at this case (no run-start stash)"})
    if now is None:
        return _R(Verdict.INCONCLUSIVE, reason="GET /status unreadable at run end; (a) and (b) clean")
    if tuple(start) != now:
        return _R(Verdict.FAIL, reason="/status (mode,state,ssid,ip_mode) changed during the run",
                  observed={"start": list(start), "end": list(now)})
    return _R(Verdict.PASS)


# ---------------------------------------------------------------------------
# WEB-SEC-02 / WEB-SEC-06
# ---------------------------------------------------------------------------

_SEC_GUARD = "Set both passwords before enabling web sign-in."


def _case_sec02(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/settings/security")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    st, cfg = _get_json(ctx, "/api/auth/config")
    if st != 200 or not isinstance(cfg, dict):
        return _R(Verdict.INCONCLUSIVE, reason=f"GET /api/auth/config unreadable (status={st})")
    problems = [f"missing {m}" for m in _missing(html, ('id="kcSecWebEnabled"', "kcSecPolicySave", _SEC_GUARD))]
    for k in ("admin_password_set", "user_password_set"):
        if k not in cfg:
            problems.append(f"/api/auth/config lacks {k}")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed={"problems": problems})
    return _R(Verdict.PASS, observed={"admin_password_set": cfg["admin_password_set"],
                                      "user_password_set": cfg["user_password_set"]})


def _case_sec06(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/settings/security")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    st, cfg = _get_json(ctx, "/api/auth/config")
    if st != 200 or not isinstance(cfg, dict) or "admin_password_set" not in cfg:
        return _R(Verdict.INCONCLUSIVE, reason=f"/api/auth/config unreadable at end (status={st})")
    # Built by concatenation so this file's own literals never match the scan.
    needle = "clear_" + "credentials"
    problems = [f"missing {m}" for m in _missing(html, ('id="kcSecClearCreds"', "Clear login credentials",
                                                        "window.kcConfirm", needle))]
    hits = scan_bench_sources(_bench_dir(ctx), forbidden_literals=(), forbidden_imports=(),
                              forbidden_extra=(needle,))
    problems += hits
    start = ctx.get("_sec06_admin_start")
    if start is None:
        ctx["_sec06_admin_start"] = cfg["admin_password_set"]
    elif start is True and cfg["admin_password_set"] is not True:
        problems.append("admin_password_set went true to false during the run")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems[:6]), observed={"problems": len(problems)})
    return _R(Verdict.PASS, observed={"admin_password_set": cfg["admin_password_set"]})


# ---------------------------------------------------------------------------
# WEB-BAK-02..04
# ---------------------------------------------------------------------------

def _export_pair(ctx: dict) -> "Tuple[Optional[str], Optional[dict], Optional[str]]":
    fn = ctx.get("backup_export")
    try:
        if fn is not None:
            raw, parsed = fn()
        else:
            from .. import backup_export_http_client as B
            host = ctx.get("host")
            if not host:
                return None, None, "no host"
            raw, parsed = B.get_export(host)
        return raw, parsed, None
    except Exception as exc:  # noqa: BLE001
        return None, None, str(exc)


def _sensitive(raw: str) -> bool:
    from .. import backup_export_http_client as B
    return B.scan_for_sensitive_fields(raw)


def _case_bak02(ctx: dict) -> CaseResult:
    st, z = _get_json(ctx, "/api/zones")
    st2, p = _get_json(ctx, "/api/profiles")
    if st != 200 or not isinstance(z, dict) or not _int(z.get("thermo_count")):
        return _R(Verdict.INCONCLUSIVE, reason=f"/api/zones unreadable (status={st})")
    plist = p.get("profiles") if isinstance(p, dict) else p
    if st2 != 200 or not isinstance(plist, list):
        return _R(Verdict.INCONCLUSIVE, reason=f"/api/profiles unreadable (status={st2})")
    raw, parsed, err = _export_pair(ctx)
    if err:
        return _R(Verdict.FAIL, reason=f"export failed or not valid JSON: {err}")
    if not isinstance(parsed, dict) or parsed.get("kind") != "kilnctl_backup" or not _int(parsed.get("version")) \
            or not isinstance(parsed.get("profiles"), list) or not isinstance(parsed.get("zones"), list):
        return _R(Verdict.FAIL, reason="export envelope wrong (kind/version/profiles/zones)")
    problems = []
    want_p = {e.get("id") for e in plist if isinstance(e, dict) and e.get("builtin") is False}
    got_p = {e.get("id") for e in parsed["profiles"] if isinstance(e, dict)}
    if want_p != got_p:
        problems.append(f"profile ids differ: missing {sorted(want_p - got_p)}, extra {sorted(got_p - want_p)}")
    want_z = set(range(z["thermo_count"]))
    got_z = {e.get("index") for e in parsed["zones"] if isinstance(e, dict)}
    if want_z != got_z:
        problems.append(f"zone indexes differ: expected {sorted(want_z)}, got {sorted(x for x in got_z if x is not None)}")
    if _sensitive(raw):
        problems.append("export contains sensitive-looking fields")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed={"problems": problems})
    return _R(Verdict.PASS, observed={"profiles": len(got_p), "zones": len(got_z)})


def _int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _canon_hash(parsed: Any) -> str:
    return hashlib.sha256(json.dumps(parsed, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _case_bak03(ctx: dict) -> CaseResult:
    """Owner 2026-10-09: reduced to export determinism; import is never POSTed."""
    skip = mutating_gate(ctx)
    if skip:
        return _R(Verdict.INCONCLUSIVE, reason=f"cannot establish a write-free window: {skip}")
    raw1, p1, e1 = _export_pair(ctx)
    if e1:
        return _R(Verdict.INCONCLUSIVE, reason=f"first export failed: {e1}")
    ctx.get("sleep", time.sleep)(ctx.get("bak03_gap_s", 5.0))
    skip = mutating_gate(ctx)
    if skip:
        return _R(Verdict.INCONCLUSIVE, reason=f"board state changed between exports: {skip}")
    raw2, p2, e2 = _export_pair(ctx)
    if e2:
        return _R(Verdict.INCONCLUSIVE, reason=f"second export failed: {e2}")
    h1, h2 = _canon_hash(p1), _canon_hash(p2)
    if h1 != h2:
        return _R(Verdict.FAIL, reason="two exports taken without writes differ", observed={"h1": h1[:12], "h2": h2[:12]})
    return _R(Verdict.PASS, observed={"hash": h1[:12], "reduction": "export determinism (owner 2026-10-09)"})


def _case_bak04(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/settings/backup")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    low = " ".join(html.lower().split())  # the page wraps this sentence across lines
    if re.search(r"refused (outright )?(\(nothing is written\) )?while a profile is running", low):
        return _R(Verdict.PASS, observed={"reduction": "static page text (owner 2026-10-09)"})
    return _R(Verdict.FAIL, reason="/settings/backup lacks the 'refused while a profile is running' text")


# ---------------------------------------------------------------------------
# WEB-KCFG-02..05
# ---------------------------------------------------------------------------

def _kcfg_snapshot(ctx: dict) -> "Tuple[Optional[dict], Optional[str]]":
    st, d = _get_json(ctx, "/api/kiln_configs")
    if st != 200 or not isinstance(d, dict) or not isinstance(d.get("configs"), list):
        return None, f"GET /api/kiln_configs unreadable (status={st})"
    return d, None


def _kcfg_key(d: dict) -> tuple:
    return (d.get("active_id"), tuple(sorted((c.get("id"), c.get("name")) for c in d["configs"])))


def _case_kcfg02(ctx: dict) -> CaseResult:
    snap, err = _kcfg_snapshot(ctx)
    if snap is None:
        return _R(Verdict.INCONCLUSIVE, reason=err)
    skip = mutating_gate(ctx)
    if skip:
        return _R(Verdict.INCONCLUSIVE, reason=skip)
    mx = snap.get("max_count")
    if not _int(mx) or mx - len(snap["configs"]) < 3:
        return _R(Verdict.INCONCLUSIVE, reason="fewer than 3 free kiln-config slots")
    created: List[int] = []
    problems: List[str] = []
    obs: Dict[str, Any] = {}

    def step(name: str, path: str, fields: dict) -> Optional[int]:
        st, b = _post_json(ctx, path, fields)
        obs[name] = st
        if st != 200 or not isinstance(b, dict) or not _int(b.get("id")):
            problems.append(f"{name} not 200/{{id}} (status={st})")
            return None
        return b["id"]

    # Review 4 H1: /api/kiln_configs/save makes the new entry the ACTIVE config
    # (kiln_cfg_store.c save_current, id<0) and the delete route refuses to delete
    # the active one, so saving is never used here. Clone/rename/export/import
    # never move active_id; start from an existing config instead.
    src = snap.get("active_id")
    if not _int(src):
        src = next((c.get("id") for c in snap["configs"] if _int(c.get("id"))), None)
    if src is None:
        return _R(Verdict.INCONCLUSIVE, reason="no existing kiln config to clone (save is not used: it activates the entry)")
    obs["save"] = "not exercised (activates the entry)"
    try:
        a = src
        if a is not None:
            b = step("clone", "/api/kiln_configs/clone", {"id": a, "name": "BENCH_tmp_c"})
            if b is not None:
                created.append(b)
                st, _ = _post_json(ctx, "/api/kiln_configs/rename", {"id": b, "name": "BENCH_tmp_r"})
                obs["rename"] = st
                if st != 200:
                    problems.append(f"rename status {st}")
                est, text = _get_text(ctx, f"/api/kiln_configs/export?id={b}")
                obs["export"] = est
                pkg = None
                if est == 200 and text:
                    try:
                        pkg = json.loads(text)
                    except ValueError:
                        pkg = None
                if pkg is None:
                    problems.append(f"export not 200/JSON (status={est})")
                else:
                    if isinstance(pkg, dict):
                        pkg["name"] = "BENCH_tmp_i"
                    ist, ib = _post_json_body(ctx, "/api/kiln_configs/import", pkg)
                    obs["import"] = ist
                    new_id = None
                    try:
                        new_id = json.loads(ib).get("id") if ib else None
                    except (ValueError, AttributeError):
                        pass
                    if ist != 200 or not _int(new_id):
                        problems.append(f"import not 200/{{id}} (status={ist})")
                    else:
                        created.append(new_id)
                        if new_id in (a, b) or new_id in [c.get("id") for c in snap["configs"]]:
                            problems.append("import returned a non-new id")
    finally:
        for cid in reversed(created):
            if cid == snap.get("active_id") or cid == src:
                continue
            _post_json(ctx, "/api/kiln_configs/delete", {"id": cid})
        after, aerr = _kcfg_snapshot(ctx)
        restored = after is not None and _kcfg_key(after) == _kcfg_key(snap)
    if not restored:
        return _R(Verdict.FAIL, reason="restore mismatch: kiln-config list differs from the pre-snapshot",
                  observed=dict(obs, created=created))
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed=obs)
    return _R(Verdict.PASS, observed=obs)


_KCFG03_STATES = ("idle", "running", "done_ok", "done_failed")


def _case_kcfg03(ctx: dict) -> CaseResult:
    st, d = _get_json(ctx, "/api/kiln_configs/apply_status")
    if st != 200 or not isinstance(d, dict):
        return _R(Verdict.FAIL, reason=f"apply_status not 200/JSON (status={st})", observed={"status": st})
    if d.get("state") not in _KCFG03_STATES or not isinstance(d.get("diverged"), bool):
        return _R(Verdict.FAIL, reason="apply_status state outside the enum or 'diverged' not a bool", observed=d)
    html, err = _html(ctx, "/settings/kiln_configs")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, ("kcApplyBtn", "apply_status"))
    if miss:
        return _R(Verdict.FAIL, reason=f"/settings/kiln_configs HTML missing {miss}")
    if d["state"] == "running":
        return _R(Verdict.INCONCLUSIVE, reason="someone else is applying (state running)")
    return _R(Verdict.PASS, observed={"state": d["state"], "reduction": "read-only (owner 2026-10-09)"})


def _case_kcfg04(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/settings/kiln_configs")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, ("kcAckHwDiffers", "X-Kiln-Ack-Hardware-Differs"))
    if miss:
        return _R(Verdict.FAIL, reason=f"/settings/kiln_configs HTML missing {miss}")
    return _R(Verdict.PASS, observed={"reduction": "static (owner 2026-10-09)"})


def _case_kcfg05(ctx: dict) -> CaseResult:
    st, body = _get_json(ctx, "/api/readiness")
    items = _unwrap_items(body)
    if st != 200 or items is None:
        return _R(Verdict.INCONCLUSIVE, reason=f"/api/readiness unreadable (status={st})")
    item = next((i for i in items if isinstance(i, dict) and i.get("key") == "safety_ceiling_match"), None)
    if item is None:
        return _R(Verdict.FAIL, reason="readiness has no safety_ceiling_match item")
    status = item.get("status")
    if status in ("cannot_yet", "deliberately_off"):
        return _R(Verdict.INCONCLUSIVE, reason=f"safety_ceiling_match is {status}")
    if status != "ok":
        return _R(Verdict.FAIL, reason=f"safety_ceiling_match is {status!r}: a real divergence",
                  observed={"status": status})
    html, err = _html(ctx, "/settings/kiln_configs")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    problems = []
    if not re.search(r'id="kcDivergeBanner"[^>]*\bhidden\b', html):
        problems.append("kcDivergeBanner missing or not hidden by default")
    if "refreshDivergence" not in html or "safety_ceiling_match" not in html:
        problems.append("refreshDivergence/safety_ceiling_match logic missing")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems))
    return _R(Verdict.PASS)


# ---------------------------------------------------------------------------
# WEB-LOG-02 / WEB-LOG-03 / WEB-X-02
# ---------------------------------------------------------------------------

def _creds(ctx: dict) -> "Tuple[Optional[str], Optional[str]]":
    import os
    return (ctx.get("web_username") or os.environ.get("KILNCTL_WEB_USERNAME"),
            ctx.get("web_password") or os.environ.get("KILNCTL_WEB_PASSWORD"))


def _open_window(client: Any, username: str, password: str, orig: dict,
                 web_timeout: Optional[int] = None) -> "Tuple[bool, str]":
    # Review 4 L3: never write the admin password unless a real login with the
    # same credential already succeeds (else the board password changes for good).
    lst, lcookie = client.login(username, password)
    if lst != 200 or not lcookie:
        return False, f"INCONCLUSIVE: login with KILNCTL_WEB_PASSWORD failed (status={lst}); no password written"
    st, resp = client.set_web_password(username, password)
    if st != 200 or not (isinstance(resp, dict) and resp.get("ok") is True):
        return False, f"set_web_password not ok (status={st})"
    st, resp = client.set_policy(True, orig["lcd_enabled"],
                                 orig["web_timeout_min"] if web_timeout is None else web_timeout,
                                 orig["lcd_timeout_min"])
    if st != 200 or not (isinstance(resp, dict) and resp.get("ok") is True):
        return False, f"set_policy(web_enabled=1) refused (status={st})"
    return True, ""


def _close_window(client: Any, orig: dict) -> bool:
    st, resp = client.set_policy(orig["web_enabled"], orig["lcd_enabled"],
                                 orig["web_timeout_min"], orig["lcd_timeout_min"])
    rs, cfg = client.get_config()
    return st == 200 and rs == 200 and isinstance(cfg, dict) and _policy_from_config(cfg) == orig


def _auth_case_prelude(ctx: dict):
    user, pw = _creds(ctx)
    if not user or not pw:
        return None, _R(Verdict.SKIP, reason="KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD not set in the environment")
    skip = mutating_gate(ctx)
    if skip:
        return None, _R(Verdict.SKIP, reason=skip)
    client = _sec_client(ctx)
    st, cfg = client.get_config()
    if st != 200 or not isinstance(cfg, dict):
        return None, _R(Verdict.INCONCLUSIVE, reason=f"GET /api/auth/config failed (status={st})")
    return (client, user, pw, _policy_from_config(cfg)), None


def _case_log02(ctx: dict) -> CaseResult:
    pre, early = _auth_case_prelude(ctx)
    if early is not None:
        return early
    client, user, pw, orig = pre
    sleep = ctx.get("sleep", time.sleep)
    verdict: CaseResult
    try:
        ok, why = _open_window(client, user, pw, orig)
        if not ok:
            verdict = _R(Verdict.INCONCLUSIVE if why.startswith("INCONCLUSIVE") else Verdict.FAIL, reason=why)
        else:
            bst, bcookie = client.login(user, "bench-wrong-credential-x")
            if bst == 429:
                verdict = _R(Verdict.INCONCLUSIVE, reason="first attempt returned 429 (existing lockout on this IP)")
            elif bst != 401 or bcookie:
                verdict = _R(Verdict.FAIL, reason=f"bad login returned {bst} / cookie={bool(bcookie)}, expected 401 and none")
            else:
                sleep(ctx.get("log02_wait_s", 6.0))
                gst, gcookie = client.login(user, pw)
                # Flags dict from _SecHttpClient.login (web_auth_login_http.c:556
                # sends "; HttpOnly; SameSite=Strict; Path=/"); never the value.
                attrs = getattr(client, "last_login_set_cookie", None)
                if gst != 200 or not gcookie:
                    verdict = _R(Verdict.FAIL, reason=f"good login returned {gst} / cookie={bool(gcookie)}")
                elif isinstance(attrs, dict) and attrs.get("httponly") is not True:
                    verdict = _R(Verdict.FAIL, reason="session cookie lacks HttpOnly")
                else:
                    sst, sess = client.get_session(gcookie)
                    if sst != 200 or not isinstance(sess, dict) or sess.get("role") in (None, "none"):
                        verdict = _R(Verdict.FAIL, reason=f"session after good login not authenticated (status={sst})")
                    else:
                        verdict = _R(Verdict.PASS, observed={"role": sess.get("role"),
                                                             "httponly_checked": isinstance(attrs, dict)})
    finally:
        restored = _close_window(client, orig)
    if not restored:
        return _R(Verdict.FAIL, reason="restore mismatch: auth policy differs from the original after the case")
    return verdict


def _case_log03(ctx: dict) -> CaseResult:
    t = (ctx.get("_results") or {}).get("WEB-SEC-05")
    if t is None or t.verdict == Verdict.NOT_RUN:
        return _R(Verdict.NOT_RUN, reason="SEC-05 did not run")
    if t.verdict == Verdict.PASS:
        return _R(Verdict.PASS, observed=dict(t.observed or {}, alias_of="WEB-SEC-05"))
    if t.verdict == Verdict.FAIL:
        return _R(Verdict.FAIL, reason=f"WEB-SEC-05 FAILed: {t.reason}", observed=t.observed or {})
    return _R(Verdict.INCONCLUSIVE, reason=f"WEB-SEC-05 was {t.verdict}: {t.reason}")


_X02_MARKERS = ("kc-lock-prompt", "Stay unlocked", "fetch('/api/auth/session/extend'", "st.prompt")


def _case_x02(ctx: dict) -> CaseResult:
    pre, early = _auth_case_prelude(ctx)
    if early is not None:
        return early
    client, user, pw, orig = pre
    sleep = ctx.get("sleep", time.sleep)
    poll_s = ctx.get("x02_poll_s", 2.0)
    verdict: CaseResult
    try:
        js, err = _html(ctx, "/app.js")
        if js is None:
            verdict = _R(Verdict.INCONCLUSIVE, reason=err or "app.js unreadable")
        elif _missing(js, _X02_MARKERS):
            verdict = _R(Verdict.FAIL, reason=f"/app.js missing {_missing(js, _X02_MARKERS)}")
        else:
            ok, why = _open_window(client, user, pw, orig, web_timeout=1)
            if not ok:
                verdict = _R(Verdict.INCONCLUSIVE if why.startswith("INCONCLUSIVE") else Verdict.FAIL, reason=why)
            else:
                lst, cookie = client.login(user, pw)
                if lst == 429:
                    verdict = _R(Verdict.INCONCLUSIVE, reason="login returned 429 (existing lockout)")
                elif lst != 200 or not cookie:
                    verdict = _R(Verdict.FAIL, reason=f"login returned {lst}")
                else:
                    verdict = _x02_run(client, cookie, sleep, poll_s, ctx.get("x02_max_polls", 30))
    finally:
        restored = _close_window(client, orig)
    if not restored:
        return _R(Verdict.FAIL, reason="restore mismatch: auth policy differs from the original after the case")
    return verdict


def _x02_run(client: Any, cookie: str, sleep, poll_s: float, max_polls: int) -> CaseResult:
    st, s = client.get_session(cookie)
    if st != 200 or not isinstance(s, dict) or s.get("prompt") is not False \
            or not isinstance(s.get("seconds_left"), (int, float)) or s["seconds_left"] <= 10:
        return _R(Verdict.FAIL, reason="early poll does not show prompt:false with seconds_left>10", observed={"poll": s})
    for _ in range(max_polls):
        sleep(poll_s)
        st, s = client.get_session(cookie)
        if not isinstance(s, dict):
            return _R(Verdict.FAIL, reason=f"session poll unreadable (status={st})")
        if s.get("role") == "none":
            return _R(Verdict.FAIL, reason="role became none before the prompt appeared")
        if s.get("prompt") is True:
            if not isinstance(s.get("seconds_left"), (int, float)) or s["seconds_left"] > 10:
                return _R(Verdict.FAIL, reason="prompt true but seconds_left>10", observed={"poll": s})
            break
    else:
        return _R(Verdict.FAIL, reason="prompt never turned true")
    est, eb = client.extend_session(cookie)
    if est != 200 or not (isinstance(eb, dict) and eb.get("ok") is True):
        return _R(Verdict.FAIL, reason=f"extend not 200 ok:true (status={est})")
    st, s = client.get_session(cookie)
    if not isinstance(s, dict) or s.get("prompt") is not False or s.get("role") in (None, "none") \
            or not isinstance(s.get("seconds_left"), (int, float)) or s["seconds_left"] < 45:
        return _R(Verdict.FAIL, reason="session not reset after extend (need prompt:false, seconds_left>=45, role!=none)",
                  observed={"poll": s})
    return _R(Verdict.PASS, observed={"role": s.get("role")})


_CASE_FUNCS = {
    "WEB-WIFI-02": _case_wifi02, "WEB-WIFI-03": _case_wifi03, "WEB-WIFI-04": _case_wifi04,
    "WEB-WIFI-05": _case_wifi05, "WEB-SEC-02": _case_sec02, "WEB-SEC-06": _case_sec06,
    "WEB-BAK-02": _case_bak02, "WEB-BAK-03": _case_bak03, "WEB-BAK-04": _case_bak04,
    "WEB-KCFG-02": _case_kcfg02, "WEB-KCFG-03": _case_kcfg03, "WEB-KCFG-04": _case_kcfg04,
    "WEB-KCFG-05": _case_kcfg05, "WEB-LOG-02": _case_log02, "WEB-LOG-03": _case_log03,
    "WEB-X-02": _case_x02,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
get_case("WEB-WIFI-05").run_start_probe = ("_wifi_status_start", _wifi_tuple)
get_case("WEB-SEC-06").run_start_probe = ("_sec06_admin_start", _sec06_admin_probe)
