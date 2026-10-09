"""WEB safety / commissioning / readiness / wizard / settings / display judges
(docs/BENCH_TEST_WEB_JUDGES_PLAN.md sections 4.4 and 4.5): WEB-SAF-02..04,
WEB-COMM-02..07, WEB-RDY-02..04, WEB-WIZ-02..11, WEB-SET-02..04,
WEB-DISP-02..04.

Rules this module follows (plan section 2/6):

* No JS runs in the harness. Each case judges the server-side inputs the
  page's JS consumes, plus the static presence of tokens in the served
  HTML/JS. Page text is fetched through the authed ``_get_text`` seam.
* Observers (SAF-03, RDY-04, COMM-04, WIZ-07/09/11, SET-04) never act on the
  board a second time: they read ``ctx["_otb01"]``/``ctx["_hp01"]``/
  ``ctx["_results"]`` or a window probe the host case fired
  (``windows.fire_window``). Their probes are read-only except RDY-04's
  ``POST /api/profile_exec/start`` with body ``probe=1`` -- no ``id`` so it
  can never start a firing (the readiness gate runs before the body is read).
* Mutating cases (COMM-07, WIZ-03, WIZ-10, DISP-02) write only inside
  ``_mutating_gate`` (mutating suite, profile executor idle, autotune not
  active), restore in ``finally``, read the restore back, and FAIL
  unconditionally when the restore does not round-trip.
* Hard limits: never ``estop_verify``, ``factory_reset``, a guard disable, a
  trip clear, a Wi-Fi/credential write, or ``abs_max_temp_c``/CT-gain
  commits. The ``cases_web_rw`` deny-list is applied by the POST seams.
* No ERROR verdict exists; an unusable probe is INCONCLUSIVE.
"""
from __future__ import annotations

import math
import re
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

from . import board_lock
from .cases_web_rw import _get_json, _get_text, _post_json, _post_raw
from .registry import CaseResult, Verdict, get_case

PASS, FAIL, INCONC, NOT_RUN = Verdict.PASS, Verdict.FAIL, Verdict.INCONCLUSIVE, Verdict.NOT_RUN


def _r(verdict: Verdict, reason: str = "", **observed: Any) -> CaseResult:
    return CaseResult(verdict, reason=reason, observed=observed or None)


# ---------------------------------------------------------------------------
# shared helpers
# ---------------------------------------------------------------------------

def _page(ctx: dict, path: str) -> Tuple[Optional[str], Optional[str]]:
    """(text, error). Authed GET of a served page or script."""
    try:
        status, text = _get_text(ctx, path)
    except Exception as exc:  # noqa: BLE001
        return None, f"GET {path} raised {exc!r}"
    if status != 200 or not text:
        return None, f"GET {path} failed (status={status})"
    return text, None


def _json(ctx: dict, path: str) -> Tuple[Optional[dict], Optional[str]]:
    try:
        status, body = _get_json(ctx, path)
    except Exception as exc:  # noqa: BLE001
        return None, f"GET {path} raised {exc!r}"
    if status != 200 or not isinstance(body, dict):
        return None, f"GET {path} failed or not JSON (status={status})"
    return body, None


def _missing(text: str, tokens: List[str]) -> List[str]:
    return [t for t in tokens if t not in text]


def _tag(html: str, element_id: str) -> Optional[str]:
    """The opening tag text carrying ``id="<element_id>"``, or None."""
    m = re.search(r'<[a-zA-Z][^>]*\bid="' + re.escape(element_id) + r'"[^>]*>', html)
    return m.group(0) if m else None


def _has_attr(tag: str, attr: str) -> bool:
    return re.search(r'(?<![\w-])' + re.escape(attr) + r'(?![\w-])', re.sub(r'"[^"]*"', '""', tag)) is not None


def _probe_value(ctx: dict, window: str, cid: str) -> Tuple[str, Any]:
    """("absent"|"missing"|"error"|"ok", value) for a window probe result."""
    windows_seen = ctx.get("_probe_results", {})
    if window not in windows_seen:
        return "absent", None
    if cid not in windows_seen[window]:
        return "missing", None
    val = windows_seen[window][cid]
    if isinstance(val, dict) and "error" in val and len(val) == 1:
        return "error", val["error"]
    return "ok", val


def _mutating_gate(ctx: dict) -> Optional[CaseResult]:
    """None when it is safe to write; else the INCONCLUSIVE to return.

    Checked immediately before the first write: mutating suite, profile
    executor idle, autotune not active (plan section 2 rule 4)."""
    suite = ctx.get("suite")
    if suite is not None and not board_lock.suite_is_mutating(suite):
        return _r(INCONC, f"gate: suite {suite!r} is not a mutating suite; no write attempted")
    pe, err = _json(ctx, "/api/profile_exec")
    if pe is None:
        return _r(INCONC, f"gate: cannot read profile_exec state ({err}); no write attempted")
    if pe.get("state") != "idle":
        return _r(INCONC, f"gate: profile_exec state is {pe.get('state')!r}, not idle; no write attempted")
    at, err = _json(ctx, "/api/autotune")
    if at is None:
        return _r(INCONC, f"gate: cannot read autotune state ({err}); no write attempted")
    if at.get("state") not in ("idle", "done", "aborted"):
        return _r(INCONC, f"gate: autotune state is {at.get('state')!r}; no write attempted")
    return None


def _ok_json(status: Optional[int], body: Any) -> bool:
    return status == 200 and isinstance(body, dict) and body.get("ok") is True


def _finite(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


# ---------------------------------------------------------------------------
# 4.4 Safety
# ---------------------------------------------------------------------------

def _diag_dict(ctx: dict) -> Dict[str, Any]:
    fn = ctx.get("safety_get_diag")
    if fn is None:
        srv = ctx.get("srv")
        if srv is None:
            from .. import mcp_server as srv  # local import: no board/MCP needed to import this module
        fn = srv._safety.get_diag
    d = fn()
    get = (lambda k: d.get(k)) if isinstance(d, dict) else (lambda k: getattr(d, k, None))
    return {
        "ever_received": get("ever_received"), "trip_mask": get("trip_mask"), "warn_mask": get("warn_mask"),
        "state": get("state"), "trip_reason": get("trip_reason"),
    }


def _saf02_pair(ctx: dict) -> Tuple[Optional[dict], Optional[dict], Optional[str]]:
    st, err = _json(ctx, "/api/status")
    if st is None:
        return None, None, err
    try:
        d = _diag_dict(ctx)
    except Exception as exc:  # noqa: BLE001
        return st, None, f"safety_get_diag failed ({type(exc).__name__}: {exc})"
    return st, d, None


def _saf02_diffs(st: dict, d: dict) -> List[str]:
    pairs = (("diag_trip_mask", "trip_mask"), ("diag_warn_mask", "warn_mask"),
             ("diag_state", "state"), ("diag_trip_reason", "trip_reason"))
    return [f"{h}: http={st.get(h)!r} uart={d.get(u)!r}" for h, u in pairs if st.get(h) != d.get(u)]


def _case_saf02(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/safety")
    if html is None:
        return _r(FAIL, err or "no /safety page")
    if 'id="diagCard"' not in html:
        return _r(FAIL, '/safety HTML has no id="diagCard"')
    samples = []
    diffs: List[str] = []
    for attempt in range(2):
        st, d, err = _saf02_pair(ctx)
        if st is None:
            return _r(FAIL, err or "status unreadable")
        if d is None:
            return _r(INCONC, f"UART diag cache unavailable: {err}")
        http_ever, uart_ever = bool(st.get("diag_ever_received")), bool(d.get("ever_received"))
        samples.append({"http": {k: st.get(k) for k in ("diag_ever_received", "diag_trip_mask", "diag_warn_mask",
                                                          "diag_state", "diag_trip_reason")}, "uart": d})
        if not http_ever and not uart_ever:
            return _r(INCONC, "both sources report diag never received (no Pico or link down)", samples=samples)
        if http_ever != uart_ever:
            diffs = [f"diag_ever_received: http={http_ever} uart={uart_ever}"]
        else:
            diffs = _saf02_diffs(st, d)
        if not diffs:
            return _r(PASS, samples=samples)
    return _r(FAIL, "Frame B card and safety_get_diag disagree on both samples: " + "; ".join(diffs), samples=samples)


_SAF03_WINDOW = "otb01_tripped"


def _saf03_probe(ctx: dict) -> dict:
    st, err = _json(ctx, "/api/status")
    if st is None:
        return {"error": err}
    return {"trip_event_ever_received": st.get("trip_event_ever_received"), "diag_trip_mask": st.get("diag_trip_mask")}


def _case_saf03(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/safety")
    if html is None:
        return _r(FAIL, err or "no /safety page")
    btn = _tag(html, "clearTripBtn")
    if btn is None:
        return _r(FAIL, 'no id="clearTripBtn" on /safety')
    if not _has_attr(btn, "disabled"):
        return _r(FAIL, "clearTripBtn lacks the static disabled attribute", tag=btn)
    otb = ctx.get("_otb01")
    if not otb:
        return _r(NOT_RUN, "OT-B01 did not run in this session")
    if otb.get("outcome") != "s6a_latched":
        return _r(NOT_RUN, f"OT-B01 outcome is {otb.get('outcome')!r}, not s6a_latched; no trip window to observe")
    kind, val = _probe_value(ctx, _SAF03_WINDOW, "WEB-SAF-03")
    if kind != "ok":
        return _r(NOT_RUN if kind in ("absent", "missing") else INCONC,
                  f"pre-clear window probe unusable ({kind}: {val})")
    mask = val.get("diag_trip_mask")
    if val.get("trip_event_ever_received") is not True:
        return _r(FAIL, "in OT-B01's trip window trip_event_ever_received is not true", window=val)
    if not _is_int(mask) or not (mask & 0x20):
        return _r(FAIL, f"in the trip window diag_trip_mask {mask!r} lacks the S6a bit 0x20", window=val)
    clear_ok = otb.get("clear_ok")
    if clear_ok is False:
        return _r(FAIL, "OT-B01 reports clear_ok false after an s6a_latched outcome", window=val)
    if clear_ok is None:
        return _r(INCONC, "OT-B01 did not report clear_ok", window=val)
    return _r(PASS, window=val, note="enabled-and-effective taken from OT-B01's own clear result; no clear POSTed here")


_SAF04_REQUIRED = ("diag_boot_reason", "diag_context_frames_ok", "diag_context_frames_bad", "diag_tx_frames_dropped")
_SAF04_OTHER = ("diag_log_frames_dropped", "safety_tc_reconfig_gave_up", "safety_s1_abs_max_disabled",
                "safety_s8_rate_guard_disabled")


def _case_saf04(ctx: dict) -> CaseResult:
    d, err = _json(ctx, "/api/status?diag=1")
    if d is None:
        return _r(FAIL, err or "unreadable")
    if d.get("ok") is not True:
        return _r(FAIL, f"ok is {d.get('ok')!r}", body=d)
    ever = d.get("diag_ever_received")
    if not isinstance(ever, bool):
        return _r(FAIL, "diag_ever_received is not a boolean", body=d)
    if not ever:
        return _r(INCONC, "diag_ever_received is false (no Pico data yet); only the flag is judged")
    bad = [k for k in _SAF04_REQUIRED if not _finite(d.get(k))]
    absent = [k for k in _SAF04_OTHER if k not in d]
    if bad:
        return _r(FAIL, f"required diag keys missing or non-numeric: {bad}", body=d)
    if absent:
        return _r(FAIL, f"diag detail keys absent: {absent}", body=d)
    return _r(PASS)


# ---------------------------------------------------------------------------
# 4.4 Commissioning
# ---------------------------------------------------------------------------

_GUIDED_CORE = (260, 529, 257, 782, 793)  # G_IDS the guided screens always show
_GUIDED_COND = (259, 258)                 # placement / borrowed zone: branch-dependent


def _case_comm02(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/settings/commissioning")
    if html is None:
        return _r(FAIL, err or "no commissioning page")
    miss = _missing(html, ['id="gStartBtn"', 'id="gCommitBtn"', "function gPrefill", "function gBuildAnswers"])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    d, err = _json(ctx, "/api/safety/commissioning")
    if d is None:
        return _r(FAIL, err or "unreadable")
    if d.get("link_up") is not True or d.get("stale") is not False or d.get("unset_reporting_reliable") is not True:
        return _r(INCONC, "link down, stale, or unset reporting unreliable",
                  link_up=d.get("link_up"), stale=d.get("stale"), reliable=d.get("unset_reporting_reliable"))
    by_id = {p.get("id"): p for p in (d.get("params") or []) if isinstance(p, dict)}
    absent = [i for i in _GUIDED_CORE + _GUIDED_COND if i not in by_id]
    if absent:
        return _r(FAIL, f"guided params absent from params[]: {absent}")
    novalue = [i for i in _GUIDED_CORE + _GUIDED_COND if by_id[i].get("set") and "value" not in by_id[i]]
    if novalue:
        return _r(FAIL, f"params set:true with no value: {novalue}")
    unset = [i for i in _GUIDED_CORE if not by_id[i].get("set")]
    if unset:
        return _r(INCONC, f"board is uncommissioned: guided params still unset: {unset}")
    return _r(PASS)


def _case_comm03(ctx: dict) -> CaseResult:
    """Read-only form (owner 2026-10-09: accepted recommendation). Never
    commits: any commit=1 would send abs_max_temp_c and the CT gains."""
    html, err = _page(ctx, "/settings/commissioning")
    if html is None:
        return _r(FAIL, err or "no commissioning page")
    miss = _missing(html, ['id="saveBtn"', "kcCommissioningCommitAndVerify"])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    d1, err = _json(ctx, "/api/safety/commissioning")
    if d1 is None:
        return _r(FAIL, err or "unreadable")
    if d1.get("link_up") is not True or d1.get("stale") is True:
        return _r(INCONC, "link down or stale", link_up=d1.get("link_up"), stale=d1.get("stale"))
    (ctx.get("_sleep") or time.sleep)(ctx.get("comm03_gap_s", 3.0))
    d2, err = _json(ctx, "/api/safety/commissioning")
    if d2 is None:
        return _r(FAIL, f"second read failed: {err}")
    if d2.get("link_up") is not True or d2.get("stale") is True:
        return _r(INCONC, "link dropped or stale on the second read")
    obs = {"crc1": d1.get("live_config_crc"), "crc2": d2.get("live_config_crc"),
           "cached": d2.get("cached_config_crc"), "commissioned": d2.get("commissioned")}
    if obs["crc1"] != obs["crc2"]:
        return _r(FAIL, f"live_config_crc drifted between reads: {obs['crc1']!r} -> {obs['crc2']!r}", **obs)
    if obs["cached"] != obs["crc2"]:
        return _r(FAIL, f"cached_config_crc {obs['cached']!r} != live_config_crc {obs['crc2']!r}", **obs)
    if d2.get("commissioned") is not True:
        return _r(FAIL, "commissioned is not true", **obs)
    return _r(PASS, **obs)


_COMM04_WINDOW = "hp01_running"


def _comm04_probe(ctx: dict) -> dict:
    pe, err = _json(ctx, "/api/profile_exec")
    if pe is None:
        return {"error": err}
    return {"state": pe.get("state")}


def _case_comm04(ctx: dict) -> CaseResult:
    if not ctx.get("_hp01"):
        return _r(NOT_RUN, "HP-01 did not run in this session")
    js, err = _page(ctx, "/commissioning_shared.js")
    if js is None:
        return _r(FAIL, err or "no commissioning_shared.js")
    miss = _missing(js, ["checkBusy", "kcCommissioningCheckBusy"])
    if miss:
        return _r(FAIL, f"static markers missing from commissioning_shared.js: {miss}")
    kind, val = _probe_value(ctx, _COMM04_WINDOW, "WEB-COMM-04")
    if kind != "ok":
        return _r(INCONC, f"HP-01's run window was not sampled ({kind}: {val})")
    if val.get("state") in ("running", "paused"):
        return _r(PASS, window=val)
    return _r(FAIL, f"during HP-01's run window profile_exec state was {val.get('state')!r}", window=val)


def _case_comm05(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/settings/commissioning")
    if html is None:
        return _r(FAIL, err or "no commissioning page")
    tag = _tag(html, "benchBtn")
    if tag is None:
        return _r(FAIL, 'no id="benchBtn" on the commissioning page')
    if not re.search(r"display:\s*none", tag):
        return _r(FAIL, "benchBtn is visible in the static HTML (no display:none)", tag=tag)
    miss = _missing(html, ["/api/safety/commissioning/bench_preset", "kcConfirm"])
    if miss:
        return _r(FAIL, f"bench_preset fetch / confirm markers missing: {miss}")
    return _r(PASS, note="benchBtn present, hidden, never pressed; zero POSTs")


_CT_CAL_MARKERS = ["ct-cal-apply", "ct-cal-auto-zero", "/api/safety/commissioning/ct_cal",
                   "/api/safety/commissioning/ct_trim", "/api/safety/commissioning/ct_auto_zero"]


def _case_comm06(ctx: dict) -> CaseResult:
    """Judge the inputs plus static markers (owner 2026-10-09: accepted
    recommendation); the per-channel DOM claim is deferred to a browser suite."""
    html, err = _page(ctx, "/settings/commissioning")
    if html is None:
        return _r(FAIL, err or "no commissioning page")
    miss = _missing(html, _CT_CAL_MARKERS)
    if miss:
        return _r(FAIL, f"CT cal markers missing: {miss}")
    d, err = _json(ctx, "/api/safety/commissioning")
    if d is None:
        return _r(FAIL, err or "unreadable")
    ct_cal = d.get("ct_cal")
    if not isinstance(ct_cal, list) or len(ct_cal) != 3:
        return _r(FAIL, f"ct_cal is not a 3-element array (got {ct_cal!r})")
    by_id = {p.get("id"): p for p in (d.get("params") or []) if isinstance(p, dict)}
    inst, topo = by_id.get(265, {}), by_id.get(799, {})
    if inst.get("set") and not inst.get("value"):
        return _r(INCONC, "ct_installed is false; no CT controls expected")
    if not topo.get("set") or topo.get("value") != 1:
        return _r(INCONC, f"ct_topology unset or not summed ({topo.get('value')!r}); ch2-only claim not applicable")
    if not (isinstance(ct_cal[2], dict) and ct_cal[2].get("has_value") is True):
        return _r(FAIL, "summed topology but ct_cal[2].has_value is not true", ct_cal=ct_cal)
    return _r(PASS, note="no POSTs exercised")


def _case_comm07(ctx: dict) -> CaseResult:
    d0, err = _json(ctx, "/api/safety/commissioning")
    if d0 is None:
        return _r(INCONC, f"cannot read commissioning ({err}); no write attempted")
    original = d0.get("relay_type")
    if original not in ("contactor", "mercury"):
        return _r(INCONC, f"relay_type {original!r} is missing/unknown; no write attempted")
    gate = _mutating_gate(ctx)
    if gate is not None:
        return gate
    path = "/api/safety/commissioning/relay_type"
    write_ok, after, restore_ok, restored = False, None, True, original
    try:
        st, body = _post_json(ctx, path, {"type": original})
        write_ok = _ok_json(st, body) and body.get("persisted") is True
        a, _ = _json(ctx, "/api/safety/commissioning")
        after = (a or {}).get("relay_type")
    finally:
        r, _ = _json(ctx, "/api/safety/commissioning")
        restored = (r or {}).get("relay_type")
        if restored != original:
            st2, body2 = _post_json(ctx, path, {"type": original})
            restore_ok = _ok_json(st2, body2)
            r2, _ = _json(ctx, "/api/safety/commissioning")
            restored = (r2 or {}).get("relay_type")
    obs = {"original": original, "after_write": after, "restored": restored}
    if not restore_ok or restored != original:
        return _r(FAIL, f"relay_type restore did not round-trip: expected {original!r}, board reads {restored!r}", **obs)
    if not write_ok or after != original:
        return _r(FAIL, f"relay_type same-value write did not round-trip (write_ok={write_ok}, read-back {after!r})", **obs)
    return _r(PASS, **obs)


# ---------------------------------------------------------------------------
# 4.4 Readiness
# ---------------------------------------------------------------------------

_RDY_STATES = ("ok", "not_done", "cannot_yet", "deliberately_off")
_RDY_HARD = ("safety_trip", "recovery_mode", "crash_report", "estop_verified")


def _items(ctx: dict) -> Tuple[Optional[List[dict]], Optional[str]]:
    d, err = _json(ctx, "/api/readiness")
    if d is None:
        return None, err
    items = d.get("items")
    if not isinstance(items, list):
        return None, "readiness has no items[]"
    return [i for i in items if isinstance(i, dict)], None


def _case_rdy02(ctx: dict) -> CaseResult:
    items, err = _items(ctx)
    if items is None:
        return _r(FAIL, err or "unreadable")
    by = {i.get("key"): i for i in items}
    absent = [k for k in _RDY_HARD if k not in by]
    if absent:
        return _r(FAIL, f"hard-interlock readiness items missing: {absent}")
    odd = [i.get("key") for i in items if i.get("status") not in _RDY_STATES]
    if odd:
        return _r(FAIL, f"item status outside the four-state set: {odd}")
    cannot = [k for k in ("safety_trip", "estop_verified") if by[k].get("status") == "cannot_yet"]
    notok = {k: by[k].get("status") for k in _RDY_HARD if by[k].get("status") != "ok"}
    bad = {k: v for k, v in notok.items() if v == "not_done"}
    if bad:
        return _r(FAIL, f"hard interlocks not ok: {bad}", statuses=notok)
    if cannot:
        return _r(INCONC, f"{cannot} are cannot_yet (safety link down?)", statuses=notok)
    if notok:
        return _r(FAIL, f"hard interlocks not ok: {notok}", statuses=notok)
    return _r(PASS)


def _case_rdy03(ctx: dict) -> CaseResult:
    items, err = _items(ctx)
    if items is None:
        return _r(FAIL, err or "unreadable")
    keys = [i.get("key") for i in items]
    bad = [k for k in keys if not isinstance(k, str) or not re.fullmatch(r"[A-Za-z0-9_\-]+", k or "")]
    if bad:
        return _r(FAIL, f"empty or non-id-safe item keys: {bad!r}")
    dup = sorted({k for k in keys if keys.count(k) > 1})
    if dup:
        return _r(FAIL, f"duplicate item keys: {dup}")
    if "safety_trip" not in keys:
        return _r(FAIL, "no safety_trip item")
    html, err = _page(ctx, "/readiness")
    if html is None:
        return _r(FAIL, err or "no /readiness page")
    miss = _missing(html, ["location.hash", "scrollIntoView", "2px solid"])
    if miss:
        return _r(FAIL, f"hash-highlight code missing from /readiness: {miss}")
    return _r(PASS, keys=keys)


_RDY04_WINDOW = "otb01_tripped"


def _rdy04_probe(ctx: dict) -> dict:
    items, err = _items(ctx)
    if items is None:
        return {"error": err}
    trip = next((i for i in items if i.get("key") == "safety_trip"), None)
    st, body = _post_raw(ctx, "/api/profile_exec/start", {"probe": "1"})
    parsed: Any = None
    try:
        import json as _json_mod
        parsed = _json_mod.loads(body) if body else None
    except (ValueError, TypeError):
        parsed = None
    return {"trip_status": (trip or {}).get("status"), "start_status": st,
            "readiness_item": (parsed or {}).get("readiness_item") if isinstance(parsed, dict) else None,
            "start_body": (body or "")[:200]}


def _case_rdy04(ctx: dict) -> CaseResult:
    otb = ctx.get("_otb01")
    if not otb:
        return _r(NOT_RUN, "OT-B01 did not run in this session")
    if otb.get("outcome") != "s6a_latched":
        return _r(NOT_RUN, f"OT-B01 outcome is {otb.get('outcome')!r}, not s6a_latched")
    kind, val = _probe_value(ctx, _RDY04_WINDOW, "WEB-RDY-04")
    if kind != "ok":
        return _r(NOT_RUN if kind in ("absent", "missing") else INCONC,
                  f"pre-clear window probe unusable ({kind}: {val})")
    if val.get("trip_status") == "cannot_yet":
        return _r(INCONC, "link went down in the window (safety_trip cannot_yet)", window=val)
    if val.get("trip_status") != "not_done":
        return _r(FAIL, f"in the trip window safety_trip is {val.get('trip_status')!r}, not not_done", window=val)
    if val.get("start_status") == 409 and val.get("readiness_item") == "safety_trip":
        return _r(PASS, window=val)
    return _r(FAIL, f"Start probe was not refused by the safety_trip gate (status {val.get('start_status')!r}, "
                    f"item {val.get('readiness_item')!r})", window=val)


# ---------------------------------------------------------------------------
# 4.5 Setup wizard
# ---------------------------------------------------------------------------

_WIZ_STEP_COUNT = 12
_WIZ_STATES = ("pending", "done", "skipped")
#: setup_wizard_page.html WIZARD_STEPS readinessKeys, by step id.
_WIZ_READINESS_KEYS = {
    0: [], 1: ["network"], 2: ["thermo_count"], 3: ["calibration"], 4: ["control_mode"],
    5: ["relays_assigned"], 6: ["guard_max_temp", "guard_cross_zone"], 7: ["safety_commissioned"],
    8: ["ct_attribution"], 9: ["autotune"], 10: ["profile_saved", "storage"], 11: [],
}


def _compute_step_state(step_id: int, stored: Optional[dict], by_key: Dict[str, dict]) -> str:
    """Python mirror of setup_wizard_page.html computeStepState (state only)."""
    keys = _WIZ_READINESS_KEYS[step_id]
    items = [by_key[k] for k in keys if k in by_key]
    stored_state = (stored or {}).get("state") or "pending"
    any_not_done = any(i.get("status") == "not_done" for i in items)
    any_cannot = any(i.get("status") == "cannot_yet" for i in items)
    all_ok = bool(items) and all(i.get("status") in ("ok", "deliberately_off") for i in items)
    if stored_state == "skipped":
        return "skipped"
    if keys and not items:
        return "unknown"
    if stored_state == "done" and (any_not_done or any_cannot):
        return "regressed"
    if stored_state == "done" or all_ok:
        return "done"
    return "pending"


def _pick_resume(states: List[str]) -> Optional[int]:
    for i, s in enumerate(states):
        if s in ("pending", "regressed", "unknown"):
            return i
    return None


def _progress(ctx: dict) -> Tuple[Optional[dict], Optional[str]]:
    return _json(ctx, "/api/setup/progress")


def _case_wiz02(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/setup")
    if html is None:
        return _r(FAIL, err or "no /setup page")
    miss = _missing(html, ['id="resumeBtn"', 'onclick="gResume()"', "function pickResumeStep"])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    prog, err = _progress(ctx)
    if prog is None:
        return _r(FAIL, err or "unreadable")
    if prog.get("version") != 1:
        return _r(FAIL, f"progress version is {prog.get('version')!r}, not 1")
    steps = prog.get("steps")
    if not isinstance(steps, dict):
        return _r(FAIL, "progress has no steps object")
    absent = [str(i) for i in range(_WIZ_STEP_COUNT) if str(i) not in steps]
    if absent:
        return _r(FAIL, f"progress step keys missing: {absent}")
    bad = {k: v.get("state") for k, v in steps.items() if not isinstance(v, dict) or v.get("state") not in _WIZ_STATES}
    if bad:
        return _r(FAIL, f"unknown step states: {bad}")
    ritems, rerr = _items(ctx)
    by_key = {i.get("key"): i for i in (ritems or [])}
    states = [_compute_step_state(i, steps[str(i)], by_key) for i in range(_WIZ_STEP_COUNT)]
    resume = _pick_resume(states)
    return _r(PASS, resume_step=resume, states=states,
              note=("wizard complete (resume id null)" if resume is None else "")
              + ("" if ritems is not None else f"; readiness unreadable ({rerr}), computed from stored state only"))


def _case_wiz03(ctx: dict) -> CaseResult:
    st0, err = _json(ctx, "/api/status")
    if st0 is None:
        return _r(INCONC, f"cannot read /api/status ({err}); no write attempted")
    orig_tz, orig_unit = st0.get("time_tz"), st0.get("temp_unit")
    if orig_unit not in ("C", "F"):
        return _r(INCONC, f"temp_unit {orig_unit!r} unreadable; no write attempted")
    tz_ok = isinstance(orig_tz, str) and orig_tz != ""
    gate = _mutating_gate(ctx)
    if gate is not None:
        return gate
    test_unit = "F" if orig_unit == "C" else "C"
    obs: Dict[str, Any] = {"orig_tz": orig_tz, "orig_unit": orig_unit}
    fail: List[str] = []
    tz_written = unit_written = False
    try:
        if tz_ok:
            tz_written = True
            s, b = _post_json(ctx, "/api/settings/tz", {"tz": "UTC0"})
            if not _ok_json(s, b):
                fail.append(f"tz write not ok (status {s})")
            a, _ = _json(ctx, "/api/status")
            obs["tz_after"] = (a or {}).get("time_tz")
            if obs["tz_after"] != "UTC0":
                fail.append(f"time_tz read back {obs['tz_after']!r}, not 'UTC0'")
        unit_written = True
        s, b = _post_json(ctx, "/api/unit_pref", {"unit": test_unit})
        if not _ok_json(s, b):
            fail.append(f"unit write not ok (status {s})")
        a, _ = _json(ctx, "/api/status")
        obs["unit_after"] = (a or {}).get("temp_unit")
        if obs["unit_after"] != test_unit:
            fail.append(f"temp_unit read back {obs['unit_after']!r}, not {test_unit!r}")
    finally:
        restore_bad: List[str] = []
        if tz_written:
            s, b = _post_json(ctx, "/api/settings/tz", {"tz": orig_tz})
            if not _ok_json(s, b):
                restore_bad.append(f"tz restore POST not ok (status {s})")
        if unit_written:
            s, b = _post_json(ctx, "/api/unit_pref", {"unit": orig_unit})
            if not _ok_json(s, b):
                restore_bad.append(f"unit restore POST not ok (status {s})")
        r, _ = _json(ctx, "/api/status")
        obs["tz_restored"], obs["unit_restored"] = (r or {}).get("time_tz"), (r or {}).get("temp_unit")
        if tz_written and obs["tz_restored"] != orig_tz:
            restore_bad.append(f"time_tz restored to {obs['tz_restored']!r}, expected {orig_tz!r}")
        if unit_written and obs["unit_restored"] != orig_unit:
            restore_bad.append(f"temp_unit restored to {obs['unit_restored']!r}, expected {orig_unit!r}")
    if restore_bad:
        return _r(FAIL, "restore did not round-trip: " + "; ".join(restore_bad), **obs)
    if fail:
        return _r(FAIL, "; ".join(fail), **obs)
    if not tz_ok:
        return _r(INCONC, "original time_tz empty/unreadable so the tz half was not run (unit half passed)", **obs)
    return _r(PASS, **obs)


def _case_wiz04(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/setup")
    if html is None:
        return _r(FAIL, err or "no /setup page")
    miss = _missing(html, ["function step2PollLive", "livereading"])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    z, err = _json(ctx, "/api/zones")
    if z is None:
        return _r(FAIL, err or "unreadable /api/zones")
    tc = z.get("thermo_count")
    if not _is_int(tc):
        return _r(FAIL, f"thermo_count is not an integer ({tc!r})")
    if tc > 3:
        return _r(FAIL, f"thermo_count {tc} exceeds THERMO_COUNT_MAX 3")
    if tc < 3:
        return _r(INCONC, f"thermo_count is {tc}; this bench is configured with fewer than three channels")
    st, err = _json(ctx, "/api/status")
    if st is None:
        return _r(FAIL, err or "unreadable /api/status")
    zones = st.get("zones")
    if not isinstance(zones, list) or len(zones) < 3:
        return _r(INCONC, "/api/status lists fewer than three zones (inactive zones are skipped)")
    bad = [i for i in range(3) if not (isinstance(zones[i], dict) and zones[i].get("actual_valid") is True
                                       and _finite(zones[i].get("actual_c")))]
    if bad:
        return _r(FAIL, f"zones {bad} lack actual_valid:true with a finite actual_c")
    return _r(PASS)


def _case_wiz05(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/setup")
    if html is None:
        return _r(FAIL, err or "no /setup page")
    if "TC_TYPES" not in html:
        return _r(FAIL, "TC_TYPES absent from /setup")
    z, err = _json(ctx, "/api/zones")
    if z is None:
        return _r(FAIL, err or "unreadable /api/zones")
    tc = z.get("thermo_count")
    if not _is_int(tc):
        return _r(FAIL, f"thermo_count is not an integer ({tc!r})")
    if tc == 0:
        return _r(INCONC, "thermo_count is 0")
    zones = z.get("zones")
    if not isinstance(zones, list) or len(zones) < tc:
        return _r(FAIL, f"zones[] shorter than thermo_count {tc}")
    types = []
    for i in range(tc):
        t, off = zones[i].get("tc_type"), zones[i].get("cal_offset_c")
        if not _is_int(t) or not 0 <= t <= 7:
            return _r(FAIL, f"zone {i} tc_type {t!r} is not an integer 0..7")
        if not _finite(off):
            return _r(FAIL, f"zone {i} cal_offset_c {off!r} is not finite")
        types.append(t)
    return _r(PASS, tc_types=types)


_WIZ06_MARKERS = ["step4ZoneTypeConfirmLines", "step6LimitConfirmLines", "kcConfirmConsequentialChange",
                  "Cancelled -- nothing was written", "/commissioning_shared.js"]


def _case_wiz06(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/setup")
    if html is None:
        return _r(FAIL, err or "no /setup page")
    miss = _missing(html, _WIZ06_MARKERS)
    if miss:
        return _r(FAIL, f"static markers missing from /setup: {miss}")
    js, err = _page(ctx, "/commissioning_shared.js")
    if js is None:
        return _r(FAIL, err or "no commissioning_shared.js")
    miss = _missing(js, ["confirmConsequentialChange", "kcConfirmConsequentialChange"])
    if miss:
        return _r(FAIL, f"commissioning_shared.js missing: {miss}")
    z, err = _json(ctx, "/api/zones")
    if z is None:
        return _r(FAIL, err or "unreadable /api/zones")
    zones = z.get("zones")
    if not isinstance(zones, list):
        return _r(FAIL, "no zones[]")
    bad = [i for i, zz in enumerate(zones) if not _is_int(zz.get("zone_type"))]
    if bad:
        return _r(FAIL, f"zones {bad} lack an integer zone_type")
    return _r(PASS, note="nothing posted")


def _alias(ctx: dict, host_id: str, html_path: str, markers: List[str]) -> CaseResult:
    res = (ctx.get("_results") or {}).get(host_id)
    if res is None:
        return _r(NOT_RUN, f"{host_id} did not run in this session")
    html, err = _page(ctx, html_path)
    if html is None:
        return _r(FAIL, err or f"no {html_path}")
    miss = _missing(html, markers)
    if res.verdict == INCONC:
        return _r(INCONC, f"{host_id} was INCONCLUSIVE: {res.reason}")
    if res.verdict != PASS:
        return _r(FAIL if res.verdict == FAIL else NOT_RUN, f"{host_id} was {res.verdict}: {res.reason}")
    if miss:
        return _r(FAIL, f"static markers missing from {html_path}: {miss}")
    return _r(PASS, note=f"alias of {host_id}; no second write")


def _case_wiz07(ctx: dict) -> CaseResult:
    return _alias(ctx, "WEB-COMM-03", "/setup", ["function renderStep7", "kcCommissioningCommitAndVerify"])


def _case_wiz08(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/setup")
    if html is None:
        return _r(FAIL, err or "no /setup page")
    miss = _missing(html, ['id="step8Ack"', 'id="step8Start"', "/api/zones/current_sweep/start", "ackBox.checked"])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    start = _tag(html, "step8Start")
    if start is None or not _has_attr(start, "disabled"):
        return _r(FAIL, "step8Start markup lacks the disabled attribute", tag=start)
    return _r(INCONC, "sweep not started by policy (needs heat with the owner present); static gating verified")


def _case_wiz09(ctx: dict) -> CaseResult:
    return _alias(ctx, "WEB-ZONE-05", "/setup", ["function renderStep9", "/api/zones/pid"])


def _case_wiz10(ctx: dict) -> CaseResult:
    snap, err = _progress(ctx)
    if snap is None:
        return _r(INCONC, f"cannot read setup progress ({err}); no write attempted")
    steps0 = snap.get("steps")
    if not isinstance(steps0, dict) or "10" not in steps0 or "11" not in steps0:
        return _r(INCONC, "progress snapshot lacks steps 10/11; no write attempted")
    gate = _mutating_gate(ctx)
    if gate is not None:
        return gate
    touched = ("10", "11")
    before = {k: {"state": steps0[k].get("state"), "note": steps0[k].get("note") or ""} for k in touched}
    obs: Dict[str, Any] = {"before": before}
    fail: List[str] = []
    cfg_unmounted = False
    written: List[str] = []
    try:
        for step, state, note in (("10", "done", ""), ("11", "skipped", "optional, deferred")):
            written.append(step)
            s, b = _post_json(ctx, "/api/setup/progress", {"step": step, "state": state, "note": note})
            if s == 503:
                cfg_unmounted = True
                break
            if not _ok_json(s, b):
                fail.append(f"step {step} write not ok (status {s})")
                continue
            p, _ = _progress(ctx)
            got = ((p or {}).get("steps") or {}).get(step) or {}
            if got.get("state") != state or (got.get("note") or "") != note:
                fail.append(f"step {step} read back {got.get('state')!r}/{got.get('note')!r}, expected {state!r}/{note!r}")
    finally:
        restore_bad: List[str] = []
        for step in written:
            s, b = _post_json(ctx, "/api/setup/progress",
                              {"step": step, "state": before[step]["state"], "note": before[step]["note"]})
            if not _ok_json(s, b) and not (cfg_unmounted and s == 503):
                restore_bad.append(f"step {step} restore POST not ok (status {s})")
        p, _ = _progress(ctx)
        for step in written:
            got = ((p or {}).get("steps") or {}).get(step) or {}
            if got.get("state") != before[step]["state"] or (got.get("note") or "") != before[step]["note"]:
                restore_bad.append(f"step {step} restored to {got.get('state')!r}/{got.get('note')!r}, "
                                   f"expected {before[step]['state']!r}/{before[step]['note']!r}")
    if restore_bad and not cfg_unmounted:
        return _r(FAIL, "restore did not round-trip (ts excluded): " + "; ".join(restore_bad), **obs)
    if cfg_unmounted:
        return _r(INCONC, "POST /api/setup/progress returned 503 (cfg not mounted)", **obs)
    if fail:
        return _r(FAIL, "; ".join(fail), **obs)
    return _r(PASS, **obs)


_WIZ11_WINDOW = "otb01_before_reset"


def _wiz11_probe(ctx: dict) -> dict:
    p, err = _progress(ctx)
    if p is None:
        return {"error": err}
    return {"steps": {k: {"state": v.get("state"), "note": v.get("note") or ""}
                      for k, v in (p.get("steps") or {}).items() if isinstance(v, dict)}}


def _case_wiz11(ctx: dict) -> CaseResult:
    otb = ctx.get("_otb01")
    if not otb:
        return _r(NOT_RUN, "OT-B01 did not run in this session")
    if not otb.get("esp_restart_confirmed"):
        return _r(INCONC, f"OT-B01 could not confirm the ESP restarted ({otb.get('outcome')!r})", otb01=otb)
    kind, val = _probe_value(ctx, _WIZ11_WINDOW, "WEB-WIZ-11")
    if kind != "ok" or not val.get("steps"):
        return _r(INCONC, f"no pre-reset progress snapshot ({kind}: {val})")
    after, err = _progress(ctx)
    if after is None:
        return _r(FAIL, f"progress unreadable after the reset: {err}")
    diffs = []
    for k, was in val["steps"].items():
        now = (after.get("steps") or {}).get(k) or {}
        if now.get("state") != was["state"] or (now.get("note") or "") != was["note"]:
            diffs.append(f"step {k}: {was['state']!r}/{was['note']!r} -> {now.get('state')!r}/{now.get('note')!r}")
    if diffs:
        return _r(FAIL, "setup progress changed across sw_reset (ts ignored): " + "; ".join(diffs))
    return _r(PASS, steps_compared=len(val["steps"]))


# ---------------------------------------------------------------------------
# 4.5 Settings
# ---------------------------------------------------------------------------

def _case_set02(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/settings")
    if html is None:
        return _r(FAIL, err or "no /settings page")
    banner = _tag(html, "cfgFsFormatBanner")
    if banner is None or 'id="cfgFsFormatConfirmBtn"' not in html:
        return _r(FAIL, "cfgFsFormatBanner / cfgFsFormatConfirmBtn missing from /settings")
    if not _has_attr(banner, "hidden"):
        return _r(FAIL, "cfgFsFormatBanner is not statically hidden", tag=banner)
    d, err = _json(ctx, "/api/cfgfs/format_pending")
    if d is None:
        return _r(FAIL, err or "unreadable")
    if not isinstance(d.get("pending"), bool):
        return _r(FAIL, f"pending is not a boolean ({d.get('pending')!r})")
    st, _ = _json(ctx, "/api/status")
    if d["pending"]:
        return _r(INCONC, f"format pending on this board: {d.get('reason')!r}; format never pressed", reason_field=d.get("reason"))
    if st and st.get("cfg_fs_format_pending") is True:
        return _r(FAIL, "/api/status says cfg_fs_format_pending true while the route says false")
    return _r(PASS)


def _case_set03(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/settings")
    if html is None:
        return _r(FAIL, err or "no /settings page")
    miss = [s for s in ("wifi", "kiln", "profiles", "all") if f'data-scope="{s}"' not in html]
    miss += _missing(html, ['id="danger"', 'id="resetStatus"'])
    if miss:
        return _r(FAIL, f"danger-zone markup missing: {miss}")
    i = html.find("querySelectorAll('.danger-btn')")
    if i < 0:
        return _r(FAIL, "no .danger-btn handler wiring")
    tail = html[i:]
    j, k = tail.find("kcConfirm("), tail.find("/api/factory_reset")
    if k < 0 or j < 0 or j > k:
        return _r(FAIL, "the factory_reset handler POSTs without a preceding kcConfirm")
    return _r(PASS, note="nothing pressed; factory_reset never called")


def _case_set04(ctx: dict) -> CaseResult:
    otb = ctx.get("_otb01")
    if not otb:
        return _r(NOT_RUN, "OT-B01 did not run in this session")
    html, err = _page(ctx, "/settings")
    if html is None:
        return _r(FAIL, err or "no /settings page")
    miss = _missing(html, ['id="swResetBtn"', 'id="swResetStatus"'])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    i = html.find("getElementById('swResetBtn')")
    tail = html[i:] if i >= 0 else ""
    j, k = tail.find("kcConfirm("), tail.find("/api/sw_reset")
    if i < 0 or j < 0 or k < 0 or j > k:
        return _r(FAIL, "the Reboot-both handler lacks kcConfirm before the /api/sw_reset POST")
    outcome = otb.get("outcome")
    if not otb.get("esp_restart_confirmed") or (outcome or "").startswith("inconclusive"):
        return _r(INCONC, f"OT-B01 could not confirm the reset ({outcome!r})", otb01=otb)
    if outcome in ("no_trip", "s6a_latched") and outcome == "s6a_latched" and otb.get("clear_ok") is False:
        return _r(FAIL, "OT-B01 latched S6a but its clear did not take effect", otb01=otb)
    if outcome in ("no_trip", "s6a_latched"):
        return _r(PASS, outcome=outcome)
    return _r(FAIL, f"OT-B01 outcome {outcome!r}: a trip other than S6a or no link after sw_reset", otb01=otb)


# ---------------------------------------------------------------------------
# 4.5 Display
# ---------------------------------------------------------------------------

_DP_PATH = "/api/settings/display_power"
_DP_FIELDS = ("brightness_percent", "timeout_setting", "keep_on_while_firing", "display_on_error")


def _dp_form(brightness: Any, timeout: Any, keep: Any, on_err: Any) -> Dict[str, Any]:
    return {"brightness": brightness, "timeout": timeout,
            "keep_on_while_firing": "1" if keep else "0", "display_on_error": "1" if on_err else "0"}


def _dp_read(ctx: dict) -> Optional[dict]:
    d, _ = _json(ctx, _DP_PATH)
    return d


def _dp_ok(status: Optional[int], body: Any) -> bool:
    return status == 200 and isinstance(body, dict) and body.get("ok", True) is not False


def _case_disp02(ctx: dict) -> CaseResult:
    snap = _dp_read(ctx)
    if snap is None or any(f not in snap for f in _DP_FIELDS):
        return _r(INCONC, "cannot read all four display_power fields; no write attempted", body=snap)
    gate = _mutating_gate(ctx)
    if gate is not None:
        return gate
    b0, t0, k0, e0 = (snap[f] for f in _DP_FIELDS)
    obs: Dict[str, Any] = {"snapshot": {f: snap[f] for f in _DP_FIELDS}, "brightness_inert": snap.get("brightness_inert")}
    fail: List[str] = []
    stash: Dict[str, Any] = {"brightness_window_ok": None, "timeout_window_ok": None, "samples": {}}
    sleep = ctx.get("_sleep") or time.sleep
    try:
        s, b = _post_json(ctx, _DP_PATH, _dp_form(50, t0, k0, e0))
        a = _dp_read(ctx) or {}
        if not _dp_ok(s, b):
            fail.append(f"brightness write not ok (status {s})")
        elif a.get("brightness_percent") != 50 or any(a.get(f) != snap[f] for f in _DP_FIELDS[1:]):
            fail.append(f"brightness=50 read back {a.get('brightness_percent')!r} or disturbed another field")
        else:
            hook = ctx.get("_lcd05_sample")
            if snap.get("brightness_inert") is True:
                stash["brightness_window_ok"] = "inconclusive: brightness_inert"
            elif hook:
                stash["samples"]["lcd05"] = hook(ctx)
                stash["brightness_window_ok"] = True
        s, b = _post_json(ctx, _DP_PATH, _dp_form(b0, 0, k0, e0))
        a = _dp_read(ctx) or {}
        if not _dp_ok(s, b):
            fail.append(f"timeout write not ok (status {s})")
        elif a.get("timeout_setting") != 0:
            fail.append(f"timeout=0 read back {a.get('timeout_setting')!r}")
        elif ctx.get("_lcd06_sample"):
            sleep(70.0)
            stash["samples"]["lcd06"] = ctx["_lcd06_sample"](ctx)
            stash["timeout_window_ok"] = True
    finally:
        s, b = _post_json(ctx, _DP_PATH, _dp_form(b0, t0, k0, e0))
        a = _dp_read(ctx) or {}
        restore_bad = [] if _dp_ok(s, b) else [f"restore POST not ok (status {s})"]
        restore_bad += [f"{f} restored to {a.get(f)!r}, expected {snap[f]!r}" for f in _DP_FIELDS if a.get(f) != snap[f]]
        ctx["_disp02"] = stash
    if restore_bad:
        return _r(FAIL, "restore did not round-trip: " + "; ".join(restore_bad), **obs)
    if fail:
        return _r(FAIL, "; ".join(fail), **obs)
    return _r(PASS, window=stash, **obs)


_DISP03_MARKERS = ['id="kcDpBrightnessNote"', "d.brightness_inert", "Applies live to the panel backlight."]


def _case_disp03(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/settings/display")
    if html is None:
        return _r(FAIL, err or "no /settings/display page")
    miss = _missing(html, _DISP03_MARKERS)
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    d = _dp_read(ctx)
    if d is None:
        return _r(FAIL, "display_power unreadable")
    inert = d.get("brightness_inert")
    if not isinstance(inert, bool):
        return _r(FAIL, f"brightness_inert is not a boolean ({inert!r})")
    board_commit, head = ctx.get("board_fw_commit"), ctx.get("worktree_head")
    if not board_commit or not head or not (str(board_commit).startswith(str(head)[:7]) or str(head).startswith(str(board_commit)[:7])):
        return _r(INCONC, "board firmware commit unknown or differs from the worktree HEAD; build flag cannot be inferred",
                  brightness_inert=inert, board_commit=board_commit, head=head)
    if inert is not False:
        return _r(FAIL, "brightness_inert is true but CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE defaults on (expected false)",
                  brightness_inert=inert)
    return _r(PASS, brightness_inert=inert)


def _case_disp04(ctx: dict) -> CaseResult:
    html, err = _page(ctx, "/settings/display")
    if html is None:
        return _r(FAIL, err or "no /settings/display page")
    miss = _missing(html, ['id="themeBtn"', 'id="kcDisplayPrefs"', "localStorage.setItem('kilnctl-theme'"])
    if miss:
        return _r(FAIL, f"static markers missing: {miss}")
    scripts = re.findall(r"<script[^>]*>(.*?)</script>", html, flags=re.S)
    theme = [s for s in scripts if "kilnctl-theme" in s]
    if not theme:
        return _r(FAIL, "no theme script block found")
    for s in theme:
        if "fetch(" in s or "/api/" in s:
            return _r(FAIL, "a theme script block calls fetch( or /api/ -- the toggle is not browser-local")
    return _r(PASS)


# ---------------------------------------------------------------------------
# wiring
# ---------------------------------------------------------------------------

_CASE_FUNCS: Dict[str, Callable[[dict], CaseResult]] = {
    "WEB-SAF-02": _case_saf02, "WEB-SAF-03": _case_saf03, "WEB-SAF-04": _case_saf04,
    "WEB-COMM-02": _case_comm02, "WEB-COMM-03": _case_comm03, "WEB-COMM-04": _case_comm04,
    "WEB-COMM-05": _case_comm05, "WEB-COMM-06": _case_comm06, "WEB-COMM-07": _case_comm07,
    "WEB-RDY-02": _case_rdy02, "WEB-RDY-03": _case_rdy03, "WEB-RDY-04": _case_rdy04,
    "WEB-WIZ-02": _case_wiz02, "WEB-WIZ-03": _case_wiz03, "WEB-WIZ-04": _case_wiz04,
    "WEB-WIZ-05": _case_wiz05, "WEB-WIZ-06": _case_wiz06, "WEB-WIZ-07": _case_wiz07,
    "WEB-WIZ-08": _case_wiz08, "WEB-WIZ-09": _case_wiz09, "WEB-WIZ-10": _case_wiz10,
    "WEB-WIZ-11": _case_wiz11,
    "WEB-SET-02": _case_set02, "WEB-SET-03": _case_set03, "WEB-SET-04": _case_set04,
    "WEB-DISP-02": _case_disp02, "WEB-DISP-03": _case_disp03, "WEB-DISP-04": _case_disp04,
}

#: observer -> (host it depends on, window it probes or None)
_OBSERVERS: Dict[str, Tuple[str, Optional[Tuple[str, Callable[[dict], dict]]]]] = {
    "WEB-SAF-03": ("OT-B01", (_SAF03_WINDOW, _saf03_probe)),
    "WEB-RDY-04": ("OT-B01", (_RDY04_WINDOW, _rdy04_probe)),
    "WEB-WIZ-11": ("OT-B01", (_WIZ11_WINDOW, _wiz11_probe)),
    "WEB-SET-04": ("OT-B01", None),
    "WEB-COMM-04": ("HP-01", (_COMM04_WINDOW, _comm04_probe)),
    "WEB-WIZ-07": ("WEB-COMM-03", None),
    "WEB-WIZ-09": ("WEB-ZONE-05", None),
}

for _cid, _fn in _CASE_FUNCS.items():
    _spec = get_case(_cid)
    _spec.judge = _fn
    if _cid in _OBSERVERS:
        _spec.window_probe = _OBSERVERS[_cid][1]
