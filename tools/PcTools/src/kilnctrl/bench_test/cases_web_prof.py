"""WEB-PROF-02..11, WEB-STIM-02 and WEB-ZONE-02..13 judges
(docs/BENCH_TEST_WEB_JUDGES_PLAN.md sections 4.2 and 4.3).

Rules followed (plan section 2): no JavaScript runs here, so each case judges
the server-side input the page's JS reads plus the static presence of the
element ids / functions in the served HTML. Board reads/writes go through the
``cases_web_rw`` seams (``_get_json``/``_post_json``/``_post_raw``/
``_post_json_body``), so fake-board tests inject ``ctx["http_get_json"]`` etc.
No ERROR verdict exists: an unrecoverable condition is INCONCLUSIVE, or FAIL
with a reason starting "ERROR:".

Owner rulings 2026-10-09 (plan section 5): WEB-STIM-02 read-only, WEB-ZONE-03
read-only, WEB-ZONE-05 identity write on the route itself, WEB-ZONE-02 strict
ceiling gate plus stripped model keys.

WEB-PROF-03..07 transient profiles (BENCH_TEST_SYSTEM_PLAN rule 12 exception,
owner 2026-10-09): HTTP accepts only ids below 100, so these cases use a FREE
USER SLOT. Before writing they read the profile list and verify the slot is
empty (``GET /api/profile?id=N`` 404 and absent from ``/api/profiles``);
INCONCLUSIVE when none is free. Profiles are named ``BT_PROF*``. In ``finally``
every id the case created is deleted and read back to confirm the slot is empty
and the user-id list equals the pre-snapshot; a leftover is a FAIL whose reason
starts "ERROR:". A slot that was not verified empty beforehand is never
deleted.
"""
from __future__ import annotations

import re
import urllib.parse
from typing import Any, Dict, List, Optional, Tuple

from . import board_lock
from . import cases_web_rw as W
from .cases_web import _web_client
from .registry import CaseResult, Verdict, get_case

_GET = W._get_json
_POST = W._post_json


# ---------------------------------------------------------------------------
# small shared helpers
# ---------------------------------------------------------------------------

def _html(ctx: dict, path: str) -> Optional[str]:
    try:
        return _web_client(ctx).goto(path)
    except Exception:  # noqa: BLE001
        return None


def _missing_tokens(html: Optional[str], tokens: List[str]) -> List[str]:
    if html is None:
        return list(tokens)
    return [t for t in tokens if t not in html]


def _is_idle_state(body: Any) -> bool:
    return isinstance(body, dict) and str(body.get("state", "")).lower() == "idle"


def _mutating_gate(ctx: dict, need_autotune_idle: bool) -> Optional[CaseResult]:
    """Standard mutating gate (plan section 2 rule 3). The suite check is
    the runner's board lock (only mutating suites hold it); this reads the
    executor (and autotune) over HTTP and SKIPs on anything but idle."""
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
    s, body = _GET(ctx, "/api/profile_exec")
    if s != 200 or not _is_idle_state(body):
        state = body.get("state") if isinstance(body, dict) else None
        return CaseResult(Verdict.SKIP, reason=(
            f"gate: executor not confirmed idle (status={s}, state={state!r}); "
            "refusing to write while a firing may be running"))
    if need_autotune_idle:
        s, body = _GET(ctx, "/api/autotune")
        if s != 200 or not _is_idle_state(body):
            state = body.get("state") if isinstance(body, dict) else None
            return CaseResult(Verdict.SKIP, reason=(
                f"gate: autotune not confirmed idle (status={s}, state={state!r})"))
    return None


def _entries(body: Any) -> Optional[List[dict]]:
    if isinstance(body, list):
        return [e for e in body if isinstance(e, dict)]
    if isinstance(body, dict):
        for key in ("profiles", "builtins", "items"):
            if isinstance(body.get(key), list):
                return [e for e in body[key] if isinstance(e, dict)]
    return None


def _num_eq(a: Any, b: Any) -> bool:
    try:
        return abs(float(a) - float(b)) < 1e-6
    except (TypeError, ValueError):
        return a == b


# ---------------------------------------------------------------------------
# WEB-PROF-02 favorite star
# ---------------------------------------------------------------------------

def _fav_ids(ctx: dict) -> Optional[List[int]]:
    s, body = _GET(ctx, "/api/profiles/favorites")
    if s != 200 or not isinstance(body, dict) or not isinstance(body.get("ids"), list):
        return None
    return list(body["ids"])


def _case_prof02(ctx: dict) -> CaseResult:
    gate = _mutating_gate(ctx, False)
    if gate:
        return gate
    s, lst = _GET(ctx, "/api/profiles")
    entries = _entries(lst)
    if s != 200 or entries is None:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/profiles unusable (status={s})")
    target = next((e.get("id") for e in entries if e.get("builtin") is True), None)
    if target is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no visible builtin profile to use as a favorite target")
    orig_ids = _fav_ids(ctx)
    if orig_ids is None:
        return CaseResult(Verdict.FAIL, reason="GET /api/profiles/favorites unusable")
    orig = target in orig_ids
    want = not orig
    obs: Dict[str, Any] = {"target": target, "orig": orig}
    problems: List[str] = []
    try:
        ws, wb = _POST(ctx, "/api/profile/favorite", {"id": str(target), "favorite": "1" if want else "0"})
        if ws != 200 or not isinstance(wb, dict) or wb.get("ok") is not True:
            problems.append(f"favorite POST not ok (status={ws}, body={wb})")
        elif wb.get("id") != target or bool(wb.get("favorite")) != want:
            problems.append(f"favorite POST echo wrong: {wb}")
        ids = _fav_ids(ctx)
        if ids is None or (target in ids) != want:
            problems.append(f"favorites ids membership did not follow the write: {ids}")
        s2, lst2 = _GET(ctx, "/api/profiles")
        e2 = _entries(lst2) or []
        if not any(e.get("id") == target for e in e2):
            problems.append("target no longer listed in /api/profiles")
    finally:
        _POST(ctx, "/api/profile/favorite", {"id": str(target), "favorite": "1" if orig else "0"})
        rids = _fav_ids(ctx)
        restored = rids is not None and ((target in rids) == orig)
        obs["restored"] = restored
    if not restored:
        return CaseResult(Verdict.FAIL, reason=(
            f"ERROR: favorite restore mismatch for profile {target}: expected membership {orig}, read {rids}"), observed=obs)
    miss = _missing_tokens(_html(ctx, "/profiles"), ["favToggleBtn", "/api/profile/favorite"])
    if miss:
        problems.append(f"served /profiles HTML missing {miss}")
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# Transient scratch-profile machinery (WEB-PROF-03..07)
# ---------------------------------------------------------------------------

_MAX_USER_SLOT = 100  # HTTP accepts ids 0..99 only


def _user_ids(lst: Any) -> Optional[List[int]]:
    entries = _entries(lst)
    if entries is None:
        return None
    out = []
    for e in entries:
        i = e.get("id")
        if isinstance(i, int) and not e.get("builtin") and 0 <= i < _MAX_USER_SLOT:
            out.append(i)
    return sorted(out)


def _user_names(lst: Any) -> List[str]:
    return [str(e.get("name")) for e in (_entries(lst) or []) if not e.get("builtin")]


def _slot_empty(ctx: dict, n: int) -> bool:
    s, _b = _GET(ctx, f"/api/profile?id={n}")
    return s == 404


def _prof_preflight(ctx: dict, names: List[str], need_free: int) -> "Tuple[Optional[CaseResult], dict]":
    gate = _mutating_gate(ctx, False)
    if gate:
        return gate, {}
    s, lst = _GET(ctx, "/api/profiles")
    pre = _user_ids(lst)
    if s != 200 or pre is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"cannot read the profile list (status={s})"), {}
    clash = [n for n in names if n in _user_names(lst)]
    if clash:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"scratch profile name(s) {clash} already exist"), {}
    by_list = [i for i in range(_MAX_USER_SLOT) if i not in pre]
    verified: List[int] = []
    for i in by_list:
        if _slot_empty(ctx, i):
            verified.append(i)
        if len(verified) >= need_free + 2:
            break
    if len(verified) < need_free:
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"no verifiably free user slot below {_MAX_USER_SLOT} "
            f"(need {need_free}, verified {len(verified)}); refusing to overwrite a user profile")), {}
    return None, {
        "pre_ids": pre, "free_by_list": by_list, "free": verified, "created": [],
        "chosen_slot": verified[0],
    }


# Wire contract (profiles_edit_http.c): POST /api/profile form fields id, name,
# zone_mask (:56), seg_count (:73), seg<i>_kind/_target/_ramp/_dwell (:105,165,177,188);
# reply {"ok":true,"id":N}. Delete: POST /api/profile/delete id=N -> 200 "ok",
# builtin id 400, unused 404 (:689-735). GET /api/profile?id=N 404 for an unused
# slot (profiles_catalog_http.c:366+); detail keys seg_kind/target_c/ramp_c_per_hr/dwell_min.
def _seg_form(segs: List[dict]) -> Dict[str, str]:
    f: Dict[str, str] = {}
    for i, sg in enumerate(segs):
        f[f"seg{i}_kind"] = str(sg.get("kind", 0))
        f[f"seg{i}_target"] = str(sg["target"])
        f[f"seg{i}_ramp"] = str(sg.get("ramp", 0))
        f[f"seg{i}_dwell"] = str(sg["dwell"])
    return f


def _create(ctx: dict, env: dict, name: str, segs: List[dict], pid: str = "-1") -> "Tuple[Optional[int], Optional[dict]]":
    fields = {"id": pid, "name": name, "zone_mask": "1", "seg_count": str(len(segs))}
    fields.update(_seg_form(segs))
    s, body = _POST(ctx, "/api/profile", fields)
    rid = body.get("id") if isinstance(body, dict) else None
    ok = s == 200 and isinstance(body, dict) and body.get("ok") is True and isinstance(rid, int)
    if ok and rid not in env["created"]:
        env["created"].append(rid)
    return (rid if ok else None), {"status": s, "body": body}


def _detail(ctx: dict, n: int) -> "Tuple[Optional[int], Optional[dict]]":
    return _GET(ctx, f"/api/profile?id={n}")


def _cleanup(ctx: dict, env: dict) -> List[str]:
    """Delete every id this case created, then confirm each slot reads 404
    and the user-id list equals the pre-snapshot. Never deletes a slot that
    was not verified empty beforehand."""
    problems: List[str] = []
    for n in list(env["created"]):
        if n not in env["free"]:
            problems.append(f"slot {n} was not verified empty before the write; NOT deleted (left in place)")
            continue
        if _slot_empty(ctx, n):
            continue
        for _attempt in range(2):
            W._post_raw(ctx, "/api/profile/delete", {"id": str(n)})
            if _slot_empty(ctx, n):
                break
        else:
            problems.append(f"slot {n} still readable after delete")
    s, lst = _GET(ctx, "/api/profiles")
    post = _user_ids(lst)
    if s != 200 or post is None:
        problems.append(f"cannot re-read /api/profiles after cleanup (status={s})")
    elif post != env["pre_ids"]:
        problems.append(f"user profile ids after cleanup {post} != before {env['pre_ids']}")
    return problems


def _run_scratch(ctx: dict, names: List[str], need_free: int, body) -> CaseResult:
    early, env = _prof_preflight(ctx, names, need_free)
    if early is not None:
        return early
    result: Optional[CaseResult] = None
    try:
        result = body(env)
    except W.ForbiddenWrite:
        raise
    except Exception as exc:  # noqa: BLE001
        result = CaseResult(Verdict.FAIL, reason=f"ERROR: case raised {type(exc).__name__}: {exc}")
    finally:
        problems = _cleanup(ctx, env)
    if problems:
        obs = dict(result.observed or {}) if result else {}
        obs.update({"cleanup_problems": problems, "slots_created": env["created"],
                    "case_verdict_before_cleanup": str(result.verdict) if result else None})
        return CaseResult(Verdict.FAIL, reason="ERROR: scratch-profile cleanup failed: " + "; ".join(problems),
                          observed=obs)
    result.observed = dict(result.observed or {})
    result.observed.setdefault("slot", env["chosen_slot"])
    return result


_SEG1 = [{"kind": 0, "target": 30, "ramp": 0, "dwell": 1}]
_SEG2 = [{"kind": 0, "target": 30, "ramp": 0, "dwell": 1}, {"kind": 0, "target": 35, "ramp": 10, "dwell": 2}]


def _seg_matches(det_seg: dict, want: dict) -> bool:
    return (_num_eq(det_seg.get("seg_kind"), want.get("kind", 0)) and _num_eq(det_seg.get("target_c"), want["target"])
            and _num_eq(det_seg.get("ramp_c_per_hr"), want.get("ramp", 0))
            and _num_eq(det_seg.get("dwell_min"), want["dwell"]))


def _case_prof03(ctx: dict) -> CaseResult:
    def body(env):
        rid, resp = _create(ctx, env, "BT_PROF03", _SEG1)
        obs = {"create": resp, "slot_expected": min(env["free_by_list"])}
        if rid is None:
            return CaseResult(Verdict.FAIL, reason=f"POST /api/profile rejected: {resp}", observed=obs)
        if rid not in env["free"]:
            return CaseResult(Verdict.FAIL, reason=f"POST returned id {rid} which was not a free slot (overwrote an occupied profile)", observed=obs)
        if rid != min(env["free_by_list"]):
            return CaseResult(Verdict.FAIL, reason=f"new profile landed in slot {rid}, not the lowest free slot {min(env['free_by_list'])}", observed=obs)
        s, det = _detail(ctx, rid)
        if s != 200 or not isinstance(det, dict):
            return CaseResult(Verdict.FAIL, reason=f"GET /api/profile?id={rid} failed (status={s})", observed=obs)
        segs = det.get("segments") or []
        if (det.get("name") != "BT_PROF03" or not _num_eq(det.get("zone_mask"), 1) or det.get("segment_count") != 1
                or len(segs) != 1 or not _seg_matches(segs[0], _SEG1[0])):
            return CaseResult(Verdict.FAIL, reason=f"profile detail does not match what was sent: {det}", observed=obs)
        miss = _missing_tokens(_html(ctx, "/profiles"), ["addSegBtn", "saveBtn"])
        if miss:
            return CaseResult(Verdict.FAIL, reason=f"served /profiles HTML missing {miss}", observed=obs)
        return CaseResult(Verdict.PASS, observed=obs)
    return _run_scratch(ctx, ["BT_PROF03"], 1, body)


def _case_prof04(ctx: dict) -> CaseResult:
    def body(env):
        rid, resp = _create(ctx, env, "BT_PROF04", _SEG1)
        obs = {"create": resp}
        if rid is None or rid not in env["free"]:
            return CaseResult(Verdict.FAIL, reason=f"create failed or landed in a non-free slot: {resp}", observed=obs)
        fields = {"id": str(rid), "name": "BT_PROF04", "zone_mask": "1", "seg_count": "2"}
        fields.update(_seg_form(_SEG2))
        s, b = _POST(ctx, "/api/profile", fields)
        obs["edit"] = {"status": s, "body": b}
        if s != 200 or not isinstance(b, dict) or b.get("ok") is not True:
            return CaseResult(Verdict.FAIL, reason=f"edit POST rejected: {b}", observed=obs)
        if b.get("id") != rid:
            if isinstance(b.get("id"), int) and b["id"] not in env["created"]:
                env["created"].append(b["id"])
            return CaseResult(Verdict.FAIL, reason=f"edit returned id {b.get('id')} instead of {rid}", observed=obs)
        s, det = _detail(ctx, rid)
        segs = (det or {}).get("segments") or []
        if s != 200 or (det or {}).get("segment_count") != 2 or len(segs) != 2 or not all(
                _seg_matches(segs[i], _SEG2[i]) for i in range(2)):
            return CaseResult(Verdict.FAIL, reason=f"detail still holds the old segments / mismatch: {det}", observed=obs)
        s, lst = _GET(ctx, "/api/profiles")
        ids = _user_ids(lst)
        if ids != sorted(env["pre_ids"] + [rid]):
            return CaseResult(Verdict.FAIL, reason=f"user id set {ids} is not pre-snapshot plus {rid}", observed=obs)
        return CaseResult(Verdict.PASS, observed=obs)
    return _run_scratch(ctx, ["BT_PROF04"], 1, body)


def _case_prof05(ctx: dict) -> CaseResult:
    def body(env):
        rid, resp = _create(ctx, env, "BT_PROF05", _SEG1)
        obs = {"create": resp}
        if rid is None or rid not in env["free"]:
            return CaseResult(Verdict.FAIL, reason=f"create failed or landed in a non-free slot: {resp}", observed=obs)
        s, text = W._post_raw(ctx, "/api/profile/delete", {"id": str(rid)})
        obs["delete"] = {"status": s, "body": text}
        if s != 200 or (text or "").strip() != "ok":
            return CaseResult(Verdict.FAIL, reason=f"delete returned status={s} body={text!r}, expected 200 'ok'", observed=obs)
        s, _b = _detail(ctx, rid)
        if s != 404:
            return CaseResult(Verdict.FAIL, reason=f"profile still readable after delete (status={s})", observed=obs)
        s, lst = _GET(ctx, "/api/profiles")
        if rid in (_user_ids(lst) or []):
            return CaseResult(Verdict.FAIL, reason="deleted id still listed in /api/profiles", observed=obs)
        bs, bt = W._post_raw(ctx, "/api/profile/delete", {"id": "128"})
        obs["builtin_delete_status"] = bs
        if bs != 400:
            return CaseResult(Verdict.FAIL, reason=f"builtin delete returned {bs}, expected 400", observed=obs)
        s, bl = _GET(ctx, "/api/profiles/builtin?all=1")
        be = _entries(bl)
        if be is not None and not any(e.get("id") == 128 for e in be):
            return CaseResult(Verdict.FAIL, reason="builtin 128 vanished after a refused delete", observed=obs)
        miss = _missing_tokens(_html(ctx, "/profiles"), ["kcConfirm", "/api/profile/delete"])
        if miss:
            return CaseResult(Verdict.FAIL, reason=f"served /profiles HTML missing {miss}", observed=obs)
        return CaseResult(Verdict.PASS, observed=obs)
    return _run_scratch(ctx, ["BT_PROF05"], 1, body)


def _case_prof06(ctx: dict) -> CaseResult:
    def body(env):
        rid, resp = _create(ctx, env, "BT_PROF06", _SEG2)
        obs = {"create": resp}
        if rid is None or rid not in env["free"]:
            return CaseResult(Verdict.FAIL, reason=f"create failed or landed in a non-free slot: {resp}", observed=obs)
        s, exp = _GET(ctx, f"/api/profile/export?id={rid}")
        if s != 200 or not isinstance(exp, dict):
            return CaseResult(Verdict.FAIL, reason=f"export failed (status={s})", observed=obs)
        if exp.get("kind") != "kilnctl_profile" or exp.get("version") != 2:
            return CaseResult(Verdict.FAIL, reason=f"export kind/version wrong: {exp.get('kind')!r}/{exp.get('version')!r}", observed=obs)
        if exp.get("name") != "BT_PROF06" or not _num_eq(exp.get("zone_mask"), 1):
            return CaseResult(Verdict.FAIL, reason="export name/zone_mask do not match what was created", observed=obs)
        _s, det = _detail(ctx, rid)
        dsegs, esegs = (det or {}).get("segments") or [], exp.get("segments") or []
        keys = ("seg_kind", "target_c", "ramp_c_per_hr", "dwell_min")
        if len(dsegs) != len(esegs) or not all(_num_eq(d.get(k), e.get(k)) for d, e in zip(dsegs, esegs) for k in keys):
            return CaseResult(Verdict.FAIL, reason=f"export segments differ from detail: {esegs} vs {dsegs}", observed=obs)
        bs, _bb = _GET(ctx, "/api/profile/export?id=128")
        if bs != 404:
            return CaseResult(Verdict.FAIL, reason=f"builtin export returned {bs}, expected 404", observed=obs)
        miss = _missing_tokens(_html(ctx, "/profiles"), ["modeExportBtn"])
        if miss:
            return CaseResult(Verdict.FAIL, reason=f"served /profiles HTML missing {miss}", observed=obs)
        return CaseResult(Verdict.PASS, observed=obs)
    return _run_scratch(ctx, ["BT_PROF06"], 1, body)


def _case_prof07(ctx: dict) -> CaseResult:
    def body(env):
        sid, resp = _create(ctx, env, "BT_PROF07", _SEG2)
        obs = {"create": resp}
        if sid is None or sid not in env["free"]:
            return CaseResult(Verdict.FAIL, reason=f"create failed or landed in a non-free slot: {resp}", observed=obs)
        s, exp = _GET(ctx, f"/api/profile/export?id={sid}")
        if s != 200 or not isinstance(exp, dict):
            return CaseResult(Verdict.FAIL, reason=f"source export failed (status={s})", observed=obs)
        imp = dict(exp)
        imp["name"] = "BT_PROF07I"
        s, text = W._post_json_body(ctx, "/api/profile/import", imp)
        body_j = W._parse_json(text) if isinstance(text, str) else text
        obs["import"] = {"status": s, "body": body_j}
        mid = body_j.get("id") if isinstance(body_j, dict) else None
        if s == 200 and isinstance(mid, int) and mid not in env["created"]:
            env["created"].append(mid)
        if s != 200 or not isinstance(body_j, dict) or body_j.get("ok") is not True or not isinstance(mid, int):
            return CaseResult(Verdict.FAIL, reason=f"import rejected: status={s} body={body_j}", observed=obs)
        if mid == sid or mid not in env["free"]:
            return CaseResult(Verdict.FAIL, reason=f"import landed in slot {mid} (source {sid}); not a previously free slot (overwrite)", observed=obs)
        _s1, d1 = _detail(ctx, sid)
        _s2, d2 = _detail(ctx, mid)
        a, b = (d1 or {}).get("segments") or [], (d2 or {}).get("segments") or []
        keys = ("seg_kind", "target_c", "ramp_c_per_hr", "dwell_min", "io_target", "io_state", "io_blocking", "io_leave_on_at_end")
        if len(a) != len(b) or not a or not all(_num_eq(x.get(k), y.get(k)) for x, y in zip(a, b) for k in keys if k in x or k in y):
            return CaseResult(Verdict.FAIL, reason=f"imported segments differ from the source: {b} vs {a}", observed=obs)
        html = _html(ctx, "/profiles")
        if html is None or html.count("/api/profile/import") < 2 or "validateImportJson" not in html:
            return CaseResult(Verdict.FAIL, reason="served /profiles HTML lacks 2x /api/profile/import or validateImportJson", observed=obs)
        return CaseResult(Verdict.PASS, observed=obs)
    return _run_scratch(ctx, ["BT_PROF07", "BT_PROF07I"], 2, body)


# ---------------------------------------------------------------------------
# WEB-PROF-08 hide builtin / 09 static / 10 relay target list / 11 user tier
# ---------------------------------------------------------------------------

def _hidden_map(ctx: dict) -> Optional[Dict[Any, bool]]:
    s, b = _GET(ctx, "/api/profiles/builtin?all=1")
    e = _entries(b)
    if s != 200 or e is None:
        return None
    return {x.get("id"): bool(x.get("hidden")) for x in e}


def _case_prof08(ctx: dict) -> CaseResult:
    gate = _mutating_gate(ctx, False)
    if gate:
        return gate
    snap = _hidden_map(ctx)
    if snap is None:
        return CaseResult(Verdict.FAIL, reason="GET /api/profiles/builtin?all=1 unusable")
    target = next((i for i, h in snap.items() if not h), None)
    if target is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="every builtin is already hidden")
    obs: Dict[str, Any] = {"target": target}
    problems: List[str] = []
    try:
        s, b = _POST(ctx, "/api/profile/builtin/hide", {"id": str(target), "hidden": "1"})
        if s != 200 or not isinstance(b, dict) or b.get("ok") is not True or not b.get("hidden"):
            problems.append(f"hide POST not ok/hidden: status={s} body={b}")
        m = _hidden_map(ctx)
        if m is None or m.get(target) is not True:
            problems.append("?all=1 does not show the target hidden")
        _s, lst = _GET(ctx, "/api/profiles")
        if any(e.get("id") == target for e in (_entries(lst) or [])):
            problems.append("hidden builtin still appears in /api/profiles")
        if not any(snap.values()):
            rs, rb = _POST(ctx, "/api/profile/builtin/restore", {})
            m2 = _hidden_map(ctx)
            obs["restore_all"] = {"status": rs, "body": rb}
            if rs != 200 or m2 is None or any(m2.values()):
                problems.append("global /restore did not unhide every builtin")
    finally:
        _POST(ctx, "/api/profile/builtin/hide", {"id": str(target), "hidden": "0"})
        fin = _hidden_map(ctx)
    if fin != snap:
        return CaseResult(Verdict.FAIL, reason=f"ERROR: builtin hidden map after restore {fin} != snapshot {snap}", observed=obs)
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_prof09(ctx: dict) -> CaseResult:
    html = _html(ctx, "/profiles")
    miss = _missing_tokens(html, ["modeDeleteBtn", "bulkActionBtn", "bulkCancelBtn", "bulkDelete", "selectionCheckbox"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /profiles HTML missing {miss}")
    s, lst = _GET(ctx, "/api/profiles")
    entries = _entries(lst)
    if s != 200 or entries is None:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/profiles unusable (status={s})")
    bad = [e.get("id") for e in entries if not isinstance(e.get("builtin"), bool)]
    if bad:
        return CaseResult(Verdict.FAIL, reason=f"profiles {bad} lack a boolean 'builtin' field")
    return CaseResult(Verdict.PASS, observed={"profiles": len(entries)})


def _case_prof10(ctx: dict) -> CaseResult:
    s, z = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(z, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    rc, names, owned = z.get("relay_count"), z.get("relay_names"), z.get("relay_zone_owned_mask")
    if not isinstance(rc, int) or not isinstance(names, list) or not isinstance(owned, int) or isinstance(owned, bool):
        return CaseResult(Verdict.FAIL, reason=f"relay_count/relay_names/relay_zone_owned_mask missing or mistyped: {rc!r}/{names!r}/{owned!r}")
    if len(names) < rc:
        return CaseResult(Verdict.FAIL, reason=f"relay_names has {len(names)} entries, fewer than relay_count {rc}")
    tc = z.get("thermo_count", len(z.get("zones") or []))
    mask = 0
    for zz in (z.get("zones") or [])[:tc]:
        rm = zz.get("relay_mask")
        if not isinstance(rm, int):
            return CaseResult(Verdict.FAIL, reason=f"zone relay_mask missing or mistyped: {zz.get('relay_mask')!r}")
        mask |= rm
    if mask != owned:
        return CaseResult(Verdict.FAIL, reason=f"relay_zone_owned_mask {owned} != OR of zone relay_mask {mask}")
    miss = _missing_tokens(_html(ctx, "/profiles"), ["ioTargetOptionsHtml", "relay_zone_owned_mask"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /profiles HTML missing {miss}")
    obs: Dict[str, Any] = {"owned_mask": owned}
    as_, aux = _GET(ctx, "/api/aux_outputs")
    if as_ != 200 or not isinstance(aux, dict) or not isinstance(aux.get("enabled_mask"), int):
        return CaseResult(Verdict.INCONCLUSIVE, reason="/api/aux_outputs unavailable; owned mask judged only", observed=obs)
    excl = owned | aux["enabled_mask"]
    obs["offered_relays"] = [i + 1 for i in range(rc) if not (excl >> i) & 1]
    return CaseResult(Verdict.PASS, observed=obs)


def _user_http(ctx: dict):
    c = ctx.get("user_http")
    if c is not None:
        return c

    class _Real:
        def __init__(self):
            self._c = W._sec_client(ctx)
            self._host = ctx.get("host")

        def login(self, u, p):
            return self._c.login(u, p)

        def session(self, cookie):
            return self._c.get_session(cookie)

        def get(self, path, cookie):
            s, t, _h = self._c._get(path, cookie)
            return s, W._parse_json(t)

        def post(self, path, fields, cookie):
            s, t, h = W._http_post_raw(self._host, path, fields, cookie=cookie)
            return s, t, h
    return _Real()


def _case_prof11(ctx: dict) -> CaseResult:
    import os
    user = ctx.get("web_user_username") or os.environ.get("KILNCTL_WEB_USER_USERNAME")
    pw = ctx.get("web_user_password") or os.environ.get("KILNCTL_WEB_USER_PASSWORD")
    if not user or not pw:
        return CaseResult(Verdict.SKIP, reason="KILNCTL_WEB_USER_USERNAME/KILNCTL_WEB_USER_PASSWORD not set")
    c = _user_http(ctx)
    s0, sess0 = c.session("") if ctx.get("user_http") is None else c.session(None)
    if s0 != 200 or not isinstance(sess0, dict) or not sess0.get("auth_enabled"):
        return CaseResult(Verdict.INCONCLUSIVE, reason="web auth is disabled on the bench; not turning it on")
    ls, cookie = c.login(user, pw)
    if ls != 200 or not cookie:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"user login did not succeed (status={ls})")
    s1, sess = c.session(cookie)
    if s1 != 200 or not isinstance(sess, dict) or sess.get("role") != "user":
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"session role is {(sess or {}).get('role')!r}, not 'user'")
    gs, lst = c.get("/api/profiles", cookie)
    if gs != 200 or _entries(lst) is None:
        return CaseResult(Verdict.FAIL, reason=f"user-tier GET /api/profiles refused or malformed (status={gs})")
    before = _user_ids(lst)
    ps, ptext, hdrs = c.post("/api/profile", {"name": "BT_PROF11"}, cookie)
    reason_hdr = None
    if hdrs is not None:
        try:
            reason_hdr = hdrs.get("X-Kiln-Auth-Reason")
        except Exception:  # noqa: BLE001
            reason_hdr = None
    obs = {"post_status": ps, "reason_header": reason_hdr}
    if ps == 200 or ps == 400:
        return CaseResult(Verdict.FAIL, reason=f"user-tier POST /api/profile got {ps}: it reached the handler instead of being refused", observed=obs)
    if ps not in (401, 403):
        return CaseResult(Verdict.FAIL, reason=f"user-tier POST /api/profile returned unexpected status {ps}", observed=obs)
    if ps == 403 and reason_hdr not in (None, "insufficient_role"):
        return CaseResult(Verdict.FAIL, reason=f"403 carried X-Kiln-Auth-Reason={reason_hdr!r}, expected insufficient_role", observed=obs)
    _s, lst2 = c.get("/api/profiles", cookie)
    if _user_ids(lst2) != before:
        return CaseResult(Verdict.FAIL, reason="profile list changed across the refused POST", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-STIM-02 (read-only per owner 2026-10-09)
# ---------------------------------------------------------------------------

def _case_stim02(ctx: dict) -> CaseResult:
    s, z = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(z, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    tc = z.get("thermo_count", 0)
    if not tc:
        return CaseResult(Verdict.INCONCLUSIVE, reason="thermo_count is 0")
    tp = z.get("timing_profiles")
    if not isinstance(tp, list) or not tp:
        return CaseResult(Verdict.FAIL, reason="timing_profiles missing or empty")
    for k, p in enumerate(tp):
        if not isinstance(p, dict) or p.get("index") != k:
            return CaseResult(Verdict.FAIL, reason=f"timing_profiles[{k}].index is not {k}: {p}")
    for i, zz in enumerate((z.get("zones") or [])[:tc]):
        t = zz.get("timing_profile")
        if not isinstance(t, int) or isinstance(t, bool) or not 0 <= t < len(tp):
            return CaseResult(Verdict.FAIL, reason=f"zone {i} timing_profile {t!r} out of range 0..{len(tp) - 1}")
    miss = _missing_tokens(_html(ctx, "/settings/safety"), ["profileOptionsHtml", "tprofile", "_timingprofile"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/safety HTML missing {miss}")
    return CaseResult(Verdict.PASS, observed={"profiles": len(tp), "zones": tc})


# ---------------------------------------------------------------------------
# WEB-ZONE-02 .. 13
# ---------------------------------------------------------------------------

_Z02_TOP_EXCLUDE = ("generation", "safety_wiring", "ct_warn_mask", "safety_ceiling", "fuzzy_model_valid")
_Z02_PREFIX_EXCLUDE = ("safety_tc_type", "normal_current_")
_Z02_OMIT_RE = re.compile(r"^(z\d+_(k|tau|deadtime)|z\d+_coupling_c\d+|coupling_diag_k_dc|.*coupling.*)$")


def _z02_strip(cfg: dict) -> dict:
    def keep(k: str) -> bool:
        return k not in _Z02_TOP_EXCLUDE and not k.startswith(_Z02_PREFIX_EXCLUDE)
    out = {k: v for k, v in cfg.items() if keep(k)}
    if isinstance(out.get("zones"), list):
        out["zones"] = [{k: v for k, v in zz.items() if keep(k)} if isinstance(zz, dict) else zz
                        for zz in out["zones"]]
    return out


def _z02_body(ctx: dict, snap: dict) -> Dict[str, str]:
    builder = ctx.get("zones_build_body")
    if builder is None:
        from .. import zones_http_client
        builder = zones_http_client.build_post_body
    raw = builder(snap, {})
    fields = dict(urllib.parse.parse_qsl(raw, keep_blank_values=True))
    return {k: v for k, v in fields.items() if not _Z02_OMIT_RE.match(k)}


def _case_zone02(ctx: dict) -> CaseResult:
    """Read-only (review 4 H2). The old identity POST re-posted the whole /api/zones
    page rebuilt from GET output (PID gains rounded to %.4f, restore compared GET to
    GET). No single field can be posted alone: gains and limits are required, so every
    valid POST re-sends rounded values. This now only proves two reads agree and the
    generation is stable; it never POSTs."""
    s, snap = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(snap, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    s1, again = _GET(ctx, "/api/zones")
    if s1 != 200 or not isinstance(again, dict):
        return CaseResult(Verdict.FAIL, reason=f"second GET /api/zones unusable (status={s1})")
    if snap.get("generation") != again.get("generation"):
        return CaseResult(Verdict.INCONCLUSIVE, reason="generation moved between reads (another writer)")
    if _z02_strip(snap) != _z02_strip(again):
        return CaseResult(Verdict.FAIL, reason="two reads at the same generation differ")
    return CaseResult(Verdict.PASS, observed={"generation": snap.get("generation"), "reduction": "read-only (review 4 H2)"})


def _case_zone03(ctx: dict) -> CaseResult:
    s, z = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(z, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    tc = z.get("thermo_count", 0)
    if not tc:
        return CaseResult(Verdict.INCONCLUSIVE, reason="thermo_count is 0")
    for i, zz in enumerate((z.get("zones") or [])[:tc]):
        for k in ("zone_type", "tc_type", "relay_type"):
            v = zz.get(k)
            if not isinstance(v, int) or isinstance(v, bool):
                return CaseResult(Verdict.FAIL, reason=f"zone {i} {k} missing or non-integer: {v!r}")
        # zones_http_get.c emits failsafe_state as a JSON bool; accept 0/1 ints too.
        fs = zz.get("failsafe_state")
        if isinstance(fs, bool):
            fs = int(fs)
        if not isinstance(fs, int):
            return CaseResult(Verdict.FAIL, reason=f"zone {i} failsafe_state missing or not bool/0-1: {zz.get('failsafe_state')!r}")
        zz = dict(zz, failsafe_state=fs)
        if zz["zone_type"] not in (0, 1) or zz["failsafe_state"] not in (0, 1):
            return CaseResult(Verdict.FAIL, reason=f"zone {i} zone_type/failsafe_state out of range: {zz['zone_type']}/{zz['failsafe_state']}")
    rt, rc = z.get("relay_types"), z.get("relay_count")
    if not isinstance(rt, list) or len(rt) != rc:
        return CaseResult(Verdict.FAIL, reason=f"relay_types length {len(rt) if isinstance(rt, list) else rt!r} != relay_count {rc!r}")
    html = _html(ctx, "/settings/zones")
    miss = _missing_tokens(html, ["tcTypeSelectHtml(", "failsafestate"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/zones HTML missing {miss}")
    return CaseResult(Verdict.PASS, observed={"zones": tc})


_GROUPS = ("limits", "relaytiming", "control", "guards", "tc")


def _case_zone04(ctx: dict) -> CaseResult:
    s, z = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(z, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    tc = z.get("thermo_count", 0)
    if not tc:
        return CaseResult(Verdict.INCONCLUSIVE, reason="thermo_count is 0")
    html = _html(ctx, "/settings/zones")
    if html is None or "groupsrc" not in html:
        return CaseResult(Verdict.FAIL, reason="served /settings/zones HTML lacks the groupsrc selectors")
    for g in _GROUPS:
        if not re.search(r"data-group\s*=\s*\\?[\"']?" + g + r"\b", html):
            return CaseResult(Verdict.FAIL, reason=f"no data-group={g} groupsrc marker in the served HTML")
    for i, zz in enumerate((z.get("zones") or [])[:tc]):
        sg = zz.get("settings_source_groups")
        if not isinstance(sg, dict):
            return CaseResult(Verdict.FAIL, reason=f"zone {i} lacks settings_source_groups")
        for g in _GROUPS:
            v = sg.get(g)
            if not isinstance(v, int) or isinstance(v, bool):
                return CaseResult(Verdict.FAIL, reason=f"zone {i} settings_source_groups.{g} missing or non-integer: {v!r}")
            if v != 255 and (not 0 <= v < tc or v == i):
                return CaseResult(Verdict.FAIL, reason=f"zone {i} settings_source_groups.{g}={v} is out of range or points at itself")
    return CaseResult(Verdict.PASS, observed={"zones": tc})


def _case_zone05(ctx: dict) -> CaseResult:
    gate = _mutating_gate(ctx, True)
    if gate:
        return gate
    s, snap = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(snap, dict) or not snap.get("zones"):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    if not snap.get("thermo_count", len(snap["zones"])):
        return CaseResult(Verdict.INCONCLUSIVE, reason="thermo_count is 0")
    z0 = snap["zones"][0]
    keys = ("pid_kp", "pid_ki", "pid_kd")
    if not all(isinstance(z0.get(k), (int, float)) for k in keys):
        return CaseResult(Verdict.FAIL, reason="zone 0 pid_kp/ki/kd missing or non-numeric")
    fields = {"zone": "0", "kp": "%.9g" % z0["pid_kp"], "ki": "%.9g" % z0["pid_ki"], "kd": "%.9g" % z0["pid_kd"]}

    def _strip(cfg):
        return {k: v for k, v in cfg.items() if k != "generation"} if isinstance(cfg, dict) else cfg

    obs: Dict[str, Any] = {"posted": fields}
    posted = False
    verdict: Optional[CaseResult] = None
    try:
        posted = True
        ps, pb = _POST(ctx, "/api/zones/pid", fields)
        if ps == 409:
            posted = False
            return CaseResult(Verdict.INCONCLUSIVE, reason="409 from system_mode_gate; nothing written", observed=obs)
        if ps != 200 or not isinstance(pb, dict) or pb.get("ok") is not True:
            verdict = CaseResult(Verdict.FAIL, reason=f"POST /api/zones/pid not ok: status={ps} body={pb}", observed=obs)
        else:
            s2, after = _GET(ctx, "/api/zones")
            a0 = (after or {}).get("zones", [{}])[0] if isinstance(after, dict) else {}
            if s2 != 200 or any("%.9g" % a0.get(k, float("nan")) != fields[n] for k, n in zip(keys, ("kp", "ki", "kd"))
                                if isinstance(a0.get(k), (int, float))) or not all(isinstance(a0.get(k), (int, float)) for k in keys):
                verdict = CaseResult(Verdict.FAIL, reason=f"gains read back differently after an identity write: {[a0.get(k) for k in keys]}", observed=obs)
            elif _strip(after) != _strip(snap):
                verdict = CaseResult(Verdict.FAIL, reason="another config field changed across the identity PID write", observed=obs)
            else:
                verdict = CaseResult(Verdict.PASS, observed=obs)
    finally:
        if posted:
            _POST(ctx, "/api/zones/pid", fields)
            _s3, fin = _GET(ctx, "/api/zones")
            f0 = (fin or {}).get("zones", [{}])[0] if isinstance(fin, dict) else {}
            if not all(f0.get(k) == z0.get(k) for k in keys):
                verdict = CaseResult(Verdict.FAIL, reason="ERROR: zone 0 PID gains restore read-back mismatch", observed=obs)
    return verdict


_ACTIVE_AT = ("stepping", "running", "settling", "analyzing", "waiting", "preparing", "tuning", "starting")


def _host_result(ctx: dict, cid: str):
    return (ctx.get("_results") or {}).get(cid)


def _case_zone06(ctx: dict) -> CaseResult:
    miss = _missing_tokens(_html(ctx, "/settings/zones"), [
        "atStartBtn", "atAbortBtn", "atAcceptBtn", "atAckUnsettled",
        "/api/autotune/start", "/api/autotune/abort", "/api/autotune/accept"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/zones HTML missing {miss}")
    hosts = {c: _host_result(ctx, c) for c in ("AT-01", "AT-02", "AT-03")}
    if not any(hosts.values()):
        return CaseResult(Verdict.NOT_RUN, reason="none of AT-01/AT-02/AT-03 ran in this invocation")
    s, st = _GET(ctx, "/api/autotune")
    state = st.get("state") if isinstance(st, dict) else None
    if s != 200 or not isinstance(state, str) or not state:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/autotune malformed (status={s}, state={state!r})")
    obs = {"state": state, "hosts": {c: (r.verdict.value if r else None) for c, r in hosts.items()}}
    for cid in ("AT-02", "AT-03"):
        r = hosts[cid]
        if r is not None and r.verdict == Verdict.PASS and state.lower() in _ACTIVE_AT:
            return CaseResult(Verdict.FAIL, reason=f"{cid} PASSed but GET /api/autotune still reports active state {state!r}", observed=obs)
    r1 = hosts["AT-01"]
    if r1 is not None and r1.verdict == Verdict.PASS and state.lower() in _ACTIVE_AT:
        return CaseResult(Verdict.FAIL, reason=f"AT-01 finished PASS but autotune is still {state!r}", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_zone07(ctx: dict) -> CaseResult:
    s, b = _GET(ctx, "/api/tuning_recommendations")
    if s != 200 or not isinstance(b, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/tuning_recommendations non-JSON or failed (status={s})")
    if b.get("schema_version") != 1:
        return CaseResult(Verdict.FAIL, reason=f"schema_version {b.get('schema_version')!r} != 1")
    recs = b.get("recommendations")
    if not isinstance(recs, list):
        return CaseResult(Verdict.FAIL, reason="recommendations missing or not a list")
    miss = _missing_tokens(_html(ctx, "/settings/zones"), ["tuningRecPanel", "tuningRecResult"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/zones HTML missing {miss}")
    if not recs:
        return CaseResult(Verdict.INCONCLUSIVE, reason="recommendations is empty (fallback artifact baked in)")
    if not any(isinstance(r, dict) and isinstance(r.get("method"), str) and r["method"] for r in recs):
        return CaseResult(Verdict.FAIL, reason="no recommendation row carries a non-empty method")
    return CaseResult(Verdict.PASS, observed={"rows": len(recs)})


def _case_zone08(ctx: dict) -> CaseResult:
    at05 = _host_result(ctx, "AT-05")
    if at05 is None:
        return CaseResult(Verdict.NOT_RUN, reason="AT-05 did not run in this invocation")
    s, b = _GET(ctx, "/api/autotune/matrix")
    if s != 200 or not isinstance(b, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/autotune/matrix failed (status={s})")
    n, cells = b.get("zone_count"), b.get("cells")
    if not isinstance(n, int) or not isinstance(cells, list) or len(cells) != n * n:
        return CaseResult(Verdict.FAIL, reason=f"cells has {len(cells) if isinstance(cells, list) else cells!r} entries, expected zone_count^2 = {n!r}**2")
    seen = set()
    for c in cells:
        if not isinstance(c, dict) or not (isinstance(c.get("i"), int) and isinstance(c.get("j"), int)):
            return CaseResult(Verdict.FAIL, reason=f"malformed cell {c}")
        seen.add((c["i"], c["j"]))
        if c.get("valid") and not all(isinstance(c.get(k), (int, float)) for k in ("k", "tau_s", "dead_time_s")):
            return CaseResult(Verdict.FAIL, reason=f"valid cell {(c['i'], c['j'])} lacks numeric k/tau_s/dead_time_s")
    if len(seen) != n * n:
        return CaseResult(Verdict.FAIL, reason="cells do not cover each (i,j) exactly once")
    rga = b.get("rga")
    if not isinstance(rga, dict):
        return CaseResult(Verdict.FAIL, reason="rga object missing")
    if rga.get("available"):
        rn, zl, lam = rga.get("n"), rga.get("zones"), rga.get("lambda")
        if (not isinstance(rn, int) or rn < 2 or not isinstance(zl, list) or len(zl) != rn
                or not isinstance(lam, list) or len(lam) != rn
                or not all(isinstance(r, list) and len(r) == rn and all(isinstance(x, (int, float)) for x in r) for r in lam)
                or not isinstance(rga.get("det"), (int, float))):
            return CaseResult(Verdict.FAIL, reason=f"rga available but malformed: {rga}")
    elif not (isinstance(rga.get("reason"), str) and rga["reason"]):
        return CaseResult(Verdict.FAIL, reason="rga unavailable without a reason string")
    miss = _missing_tokens(_html(ctx, "/settings/zones"), ["couplingMatrix", "rgaMatrix"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/zones HTML missing {miss}")
    if at05.verdict == Verdict.PASS and n and not any(c.get("valid") for c in cells):
        return CaseResult(Verdict.FAIL, reason="AT-05 PASSed but every matrix cell is valid:false")
    return CaseResult(Verdict.PASS, observed={"zone_count": n})


def _case_zone09(ctx: dict) -> CaseResult:
    gate = _mutating_gate(ctx, True)
    if gate:
        return gate

    def en(body):
        zs = body.get("zones") if isinstance(body, dict) else None
        if not isinstance(zs, list):
            return None
        z = next((x for x in zs if isinstance(x, dict) and x.get("index", 0) == 0), None) or (zs[0] if zs else None)
        return bool(z.get("enabled")) if isinstance(z, dict) and "enabled" in z else None

    def gains():
        s, z = _GET(ctx, "/api/zones")
        z0 = (z or {}).get("zones", [{}])[0] if isinstance(z, dict) else {}
        return [z0.get(k) for k in ("pid_kp", "pid_ki", "pid_kd", "model_k_dc")]

    s, b = _GET(ctx, "/api/adaptive_tune")
    orig = en(b)
    if s != 200 or orig is None:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/adaptive_tune unusable (status={s})")
    g0 = gains()
    obs: Dict[str, Any] = {"orig_enabled": orig, "revert": "not exercised (revert rewrites gains)"}
    verdict: Optional[CaseResult] = None
    wrote = False
    try:
        ws, wb = _POST(ctx, "/api/adaptive_tune/enable", {"zone": "0", "enabled": "0" if orig else "1"})
        if ws == 409:
            return CaseResult(Verdict.INCONCLUSIVE, reason="409 from system_mode_gate; nothing written", observed=obs)
        wrote = True
        if ws != 200 or not isinstance(wb, dict) or wb.get("ok") is not True:
            verdict = CaseResult(Verdict.FAIL, reason=f"enable POST not ok: status={ws} body={wb}", observed=obs)
        else:
            _s, b2 = _GET(ctx, "/api/adaptive_tune")
            if en(b2) != (not orig):
                verdict = CaseResult(Verdict.FAIL, reason=f"read-back enabled={en(b2)}, expected {not orig}", observed=obs)
    finally:
        if wrote:
            _POST(ctx, "/api/adaptive_tune/enable", {"zone": "0", "enabled": "1" if orig else "0"})
            _s, b3 = _GET(ctx, "/api/adaptive_tune")
            if en(b3) != orig:
                verdict = CaseResult(Verdict.FAIL, reason=f"ERROR: adaptive_tune enabled restore mismatch: expected {orig}, read {en(b3)}", observed=obs)
    if verdict is None:
        if gains() != g0:
            return CaseResult(Verdict.FAIL, reason="zone 0 gains changed across the enable toggle", observed=obs)
        verdict = CaseResult(Verdict.PASS, observed=obs)
    return verdict


def _case_zone10(ctx: dict) -> CaseResult:
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
    miss = _missing_tokens(_html(ctx, "/settings/zones"), ["sweepStartBtn", "sweepAbortBtn"])
    s, st = _GET(ctx, "/api/zones/current_sweep/status")
    state = st.get("state") if isinstance(st, dict) else None
    if s != 200 or state not in ("idle", "done", "aborted", "failed", "running"):
        return CaseResult(Verdict.FAIL, reason=f"current_sweep status malformed (status={s}, state={state!r})")
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/zones HTML missing {miss}")
    if state == "running":
        return CaseResult(Verdict.INCONCLUSIVE, reason="a sweep is already running; not aborting another owner's sweep")
    # Never POST start: the sweep derives and pushes k_ct (CT calibration).
    ps, pb = _POST(ctx, "/api/zones/current_sweep/abort", {})
    if ps != 200 or not isinstance(pb, dict) or pb.get("ok") is not True:
        return CaseResult(Verdict.FAIL, reason=f"abort with admin session returned status={ps} body={pb}")
    _s, st2 = _GET(ctx, "/api/zones/current_sweep/status")
    if isinstance(st2, dict) and st2.get("state") == "running":
        return CaseResult(Verdict.FAIL, reason="sweep state became running after the abort")
    return CaseResult(Verdict.PASS, observed={"state": state, "start": "not exercised: writes CT calibration"})


def _fs_counts(zones: Any) -> List[int]:
    """sample_count of each ``zones[].firing_stats`` entry (exact path).

    Wire contract: dashboard_json.c:196-205 (/api/profile_exec, top-level
    ``zones`` array, non-control shape) and dashboard_json.c:485-494
    (/api/firing_history: ``records[].zones[].firing_stats``); both emit
    ``"sample_count":%lu``.
    """
    out: List[int] = []
    for z in zones if isinstance(zones, list) else []:
        fs = z.get("firing_stats") if isinstance(z, dict) else None
        sc = fs.get("sample_count") if isinstance(fs, dict) else None
        if isinstance(sc, int) and not isinstance(sc, bool):
            out.append(sc)
    return out


def _history_counts(fh: Any) -> List[int]:
    out: List[int] = []
    for rec in (fh.get("records") if isinstance(fh, dict) else None) or []:
        out += _fs_counts(rec.get("zones") if isinstance(rec, dict) else None)
    return out


def _case_zone11(ctx: dict) -> CaseResult:
    hp = _host_result(ctx, "HP-01")
    if hp is None:
        return CaseResult(Verdict.NOT_RUN, reason="HP-01 did not run in this invocation")
    miss = _missing_tokens(_html(ctx, "/settings/zones"), ["tuningQuality", "firingStatsCurrent", "firingStatsHistory"])
    if miss:
        return CaseResult(Verdict.FAIL, reason=f"served /settings/zones HTML missing {miss}")
    s, z = _GET(ctx, "/api/zones")
    if s != 200 or not isinstance(z, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones unusable (status={s})")
    tc = z.get("thermo_count", len(z.get("zones") or []))
    valid_any = False
    for i, zz in enumerate((z.get("zones") or [])[:tc]):
        tv = zz.get("tuning_valid")
        if tv is not None and not isinstance(tv, (bool, int)):
            return CaseResult(Verdict.FAIL, reason=f"zone {i} tuning_valid malformed: {tv!r}")
        if tv:
            valid_any = True
            if not isinstance(zz.get("method"), (str, int)) or not isinstance(zz.get("settled", False), (bool, int)):
                return CaseResult(Verdict.FAIL, reason=f"zone {i} tuning_* fields malformed")
    _s, pe = _GET(ctx, "/api/profile_exec")
    counts = _fs_counts(pe.get("zones") if isinstance(pe, dict) else None)
    pid = (ctx.get("_hp01") or {}).get("profile_id") if isinstance(ctx.get("_hp01"), dict) else None
    _s, fh = _GET(ctx, "/api/firing_history" + (f"?profile_id={pid}" if pid is not None else ""))
    hist = _history_counts(fh)
    obs = {"profile_exec_counts": counts, "history_counts": hist}
    if hp.verdict == Verdict.PASS and not any(c > 0 for c in counts + hist):
        return CaseResult(Verdict.FAIL, reason="HP-01 PASSed but every firing_stats has sample_count 0 or is absent", observed=obs)
    if not valid_any:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no zone has tuning_valid; tuning-quality section gated off", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


_DIAG_KEYS = tuple(f"coupling_tau_c{i}" for i in range(3)) + tuple(f"coupling_dead_time_c{i}" for i in range(3)) + (
    "model_fit_temp_c", "model_fit_ambient_c")


def _case_zone12(ctx: dict) -> CaseResult:
    s, d = _GET(ctx, "/api/zones_diag")
    if s != 200 or not isinstance(d, dict) or d.get("ok") is False:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones_diag failed (status={s})")
    gen = d.get("generation")
    if not isinstance(gen, int) or isinstance(gen, bool):
        return CaseResult(Verdict.FAIL, reason=f"generation missing or non-integer: {gen!r}")
    zs = d.get("zones")
    if not isinstance(zs, list) or len(zs) != 3:
        return CaseResult(Verdict.FAIL, reason=f"zones length {len(zs) if isinstance(zs, list) else zs!r}, expected 3")
    for i, zz in enumerate(zs):
        if not isinstance(zz, dict) or zz.get("index") != i:
            return CaseResult(Verdict.FAIL, reason=f"zones[{i}].index is not {i}")
        for k in _DIAG_KEYS:
            v = zz.get(k)
            if not isinstance(v, (int, float)) or isinstance(v, bool):
                return CaseResult(Verdict.FAIL, reason=f"zones[{i}].{k} missing or non-numeric: {v!r}")
    sz, z = _GET(ctx, "/api/zones")
    if sz == 200 and isinstance(z, dict):
        if z.get("generation") != gen:
            return CaseResult(Verdict.INCONCLUSIVE, reason=f"generation differs from adjacent GET /api/zones ({gen} vs {z.get('generation')})")
        try:
            from .. import zones_http_client
            zones_http_client.merge_zones_diag(z, d)
        except Exception as exc:  # noqa: BLE001
            return CaseResult(Verdict.FAIL, reason=f"merge_zones_diag raised {type(exc).__name__}: {exc}")
    return CaseResult(Verdict.PASS, observed={"generation": gen})


def _case_zone13(ctx: dict) -> CaseResult:
    s, b = _GET(ctx, "/api/zones/ct_channel_map")
    if s != 200 or not isinstance(b, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones/ct_channel_map failed (status={s})")
    for k in ("mask", "committed_mask", "k_mask"):
        if not isinstance(b.get(k), int) or isinstance(b.get(k), bool):
            return CaseResult(Verdict.FAIL, reason=f"{k} missing or non-integer: {b.get(k)!r}")
    for k in ("zone", "committed_zone", "k"):
        if not isinstance(b.get(k), list) or len(b[k]) != 3:
            return CaseResult(Verdict.FAIL, reason=f"{k} missing or not length 3: {b.get(k)!r}")
    if b["committed_mask"] == 0:
        return CaseResult(Verdict.INCONCLUSIVE, reason="committed_mask is 0: Pico not commissioned")
    both = b["mask"] & b["committed_mask"]
    if both == 0:
        return CaseResult(Verdict.INCONCLUSIVE, reason="mask & committed_mask is 0: no sweep run, nothing to compare")
    cmp_n = 0
    for c in range(3):
        if (both >> c) & 1:
            cmp_n += 1
            if b["zone"][c] != b["committed_zone"][c]:
                return CaseResult(Verdict.FAIL, reason=f"channel {c}: zone {b['zone'][c]} != committed_zone {b['committed_zone'][c]}")
    return CaseResult(Verdict.PASS, observed={"channels_compared": cmp_n})


_CASE_FUNCS = {
    "WEB-PROF-02": _case_prof02, "WEB-PROF-03": _case_prof03, "WEB-PROF-04": _case_prof04,
    "WEB-PROF-05": _case_prof05, "WEB-PROF-06": _case_prof06, "WEB-PROF-07": _case_prof07,
    "WEB-PROF-08": _case_prof08, "WEB-PROF-09": _case_prof09, "WEB-PROF-10": _case_prof10,
    "WEB-PROF-11": _case_prof11, "WEB-STIM-02": _case_stim02,
    "WEB-ZONE-02": _case_zone02, "WEB-ZONE-03": _case_zone03, "WEB-ZONE-04": _case_zone04,
    "WEB-ZONE-05": _case_zone05, "WEB-ZONE-06": _case_zone06, "WEB-ZONE-07": _case_zone07,
    "WEB-ZONE-08": _case_zone08, "WEB-ZONE-09": _case_zone09, "WEB-ZONE-10": _case_zone10,
    "WEB-ZONE-11": _case_zone11, "WEB-ZONE-12": _case_zone12, "WEB-ZONE-13": _case_zone13,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
