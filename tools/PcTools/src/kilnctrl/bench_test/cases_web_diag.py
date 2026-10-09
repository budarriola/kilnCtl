"""WEB-DIAG-02..06/09..11 and WEB-OTA-02..08 judges
(docs/BENCH_TEST_WEB_JUDGES_PLAN.md section 4.6).

No JavaScript runs here: a case judges the server-side input the page's JS
reads plus the static presence of the element ids / JS functions in the served
HTML (plan section 2 rule 1). Board reads go through ``cases_web_rw._get_json``
and page HTML through ``cases_web._web_client(ctx).goto`` so fake-board tests
inject ``ctx["http_get_json"]`` / ``ctx["web_client"]``. No case here pushes an
image, presses Ack/Clear/Reset, or POSTs anything except the one inert Danger
Mode refusal probe in WEB-DIAG-09. No ERROR verdict is used: an environment
problem is INCONCLUSIVE, a disagreement is FAIL.
"""
from __future__ import annotations

import csv
import io
import os
import re
from typing import Any, Dict, List, Optional, Tuple

from . import board_lock
from .cases_web import _web_client
from .cases_web_rw import _get_json, _post_json
from .registry import CaseResult, Verdict, get_case

_R = CaseResult


def _html(ctx: dict, path: str) -> "Tuple[Optional[str], Optional[str]]":
    try:
        return _web_client(ctx).goto(path), None
    except Exception as exc:  # noqa: BLE001
        return None, f"GET {path} failed: {exc}"


def _missing(html: str, markers) -> List[str]:
    return [m for m in markers if m not in html]


def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _is_num(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def mutating_gate(ctx: dict) -> Optional[str]:
    """Standard mutating gate (plan section 2 rule 3): returns a SKIP reason,
    or None when the case may write. Checked immediately before a first write."""
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return refusal
    st, ex = _get_json(ctx, "/api/profile_exec")
    if st != 200 or not isinstance(ex, dict):
        return f"GET /api/profile_exec unreadable (status={st}); cannot confirm the executor is idle"
    if ex.get("state") != "idle":
        return f"profile executor is {ex.get('state')!r}, not idle"
    st, at = _get_json(ctx, "/api/autotune")
    if st != 200 or not isinstance(at, dict):
        return f"GET /api/autotune unreadable (status={st}); cannot confirm autotune is inactive"
    if at.get("state") not in ("idle", "done", "complete", "failed", "aborted", "accepted"):
        return f"autotune is active (state={at.get('state')!r})"
    return None


# ---------------------------------------------------------------------------
# WEB-DIAG-02
# ---------------------------------------------------------------------------

def _case_diag02(ctx: dict) -> CaseResult:
    st, body = _get_json(ctx, "/api/crash_report")
    if st != 200 or not isinstance(body, dict):
        return _R(Verdict.FAIL, reason=f"GET /api/crash_report not 200/JSON (status={st})", observed={"status": st})
    present = body.get("present")
    if not isinstance(present, bool):
        return _R(Verdict.FAIL, reason="crash_report 'present' missing or not a bool", observed={"body": body})
    # An acknowledged record is reviewed board state: the page is still
    # checked. Only a present-but-unacknowledged record (or an unreadable
    # flag) stays INCONCLUSIVE, since this case may not ack/clear it.
    if present and body.get("acknowledged") is not True:
        return _R(Verdict.INCONCLUSIVE,
                  reason="an unacknowledged crash is on record (board state); this case may not ack/clear it",
                  observed={"present": True, "acknowledged": body.get("acknowledged")})
    html, err = _html(ctx, "/diagnostics")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, ('id="crashCard"', "crashAckBtn", "crashClearBtn"))
    if miss:
        return _R(Verdict.FAIL, reason=f"/diagnostics HTML missing {miss}", observed={"missing": miss})
    return _R(Verdict.PASS, observed={"present": present, "acknowledged": bool(present)})


# ---------------------------------------------------------------------------
# WEB-DIAG-03 -- partitions equal the reference table
# ---------------------------------------------------------------------------

_APP_SUBTYPES = {"factory": 0x00, "ota_0": 0x10, "ota_1": 0x11, "test": 0x20}
_DATA_SUBTYPES = {"ota": 0x00, "phy": 0x01, "nvs": 0x02, "coredump": 0x03, "spiffs": 0x82,
                  "littlefs": 0x83, "undefined": 0x06}


def _num(text: str) -> Optional[int]:
    try:
        return int(text.strip(), 0)
    except ValueError:
        return None


def parse_partitions_csv(text: str) -> List[Tuple[str, Optional[int], Optional[int], int, int]]:
    """(label, type, subtype or None if unmapped, offset, size) per csv row.
    Rows with a blank offset/size cannot be compared and raise ValueError."""
    rows = []
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    for rec in csv.reader(io.StringIO("\n".join(lines))):
        rec = [c.strip() for c in rec]
        if len(rec) < 5:
            continue
        label, ptype, sub, off, size = rec[:5]
        ptype_n = {"app": 0, "data": 1}.get(ptype, _num(ptype))
        sub_n = (_APP_SUBTYPES if ptype == "app" else _DATA_SUBTYPES).get(sub, _num(sub))
        o, s = _num(off), _num(size)
        if o is None or s is None:
            raise ValueError(f"partition {label!r} has a blank/unparsable offset or size")
        rows.append((label, ptype_n, sub_n, o, s))
    return rows


def _case_diag03(ctx: dict) -> CaseResult:
    ref = ctx.get("_fl01_partition_list")
    if ref is None:
        path = ctx.get("partitions_csv_path") or os.path.join(
            ctx.get("repo_root") or "", "firmware", "KilnFW", "partitions.csv")
        try:
            with open(path, "r", encoding="utf-8") as f:
                ref = parse_partitions_csv(f.read())
        except (OSError, ValueError) as exc:
            return _R(Verdict.INCONCLUSIVE, reason=f"reference partitions.csv unavailable: {exc}")
    st, body = _get_json(ctx, "/api/partitions")
    if st != 200 or not isinstance(body, dict) or not isinstance(body.get("partitions"), list) \
            or not body["partitions"]:
        return _R(Verdict.FAIL, reason=f"GET /api/partitions not 200 with a non-empty list (status={st})",
                  observed={"status": st})
    keys = ("label", "type", "subtype", "offset", "size", "encrypted")
    got = {}
    for e in body["partitions"]:
        if not isinstance(e, dict) or any(k not in e for k in keys):
            return _R(Verdict.FAIL, reason=f"partition entry lacks one of {keys}: {e!r}")
        got[e["label"]] = e
    problems = []
    refd = {r[0]: r for r in ref}
    for label in sorted(set(refd) - set(got)):
        problems.append(f"missing {label}")
    for label in sorted(set(got) - set(refd)):
        problems.append(f"extra {label}")
    for label, (_l, t, sub, off, size) in refd.items():
        e = got.get(label)
        if e is None:
            continue
        if e["offset"] != off or e["size"] != size:
            problems.append(f"{label}: offset/size {e['offset']:#x}/{e['size']:#x} != {off:#x}/{size:#x}")
        if t is not None and e["type"] != t:
            problems.append(f"{label}: type {e['type']} != {t}")
        if sub is not None and e["subtype"] != sub:
            problems.append(f"{label}: subtype {e['subtype']} != {sub}")
    running = body.get("running")
    app_labels = {e["label"] for e in body["partitions"] if e.get("type") == 0}
    if running not in app_labels:
        problems.append(f"running {running!r} is not a listed app partition")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed={"problems": problems})
    return _R(Verdict.PASS, observed={"running": running, "count": len(got)})


# ---------------------------------------------------------------------------
# WEB-DIAG-04 -- cfgfs card equals FL-07
# ---------------------------------------------------------------------------

def _cfgfs_stable(d: dict) -> dict:
    cap = d.get("capacity") if isinstance(d.get("capacity"), dict) else {}
    items = (d.get("dual_write") or {}).get("items") if isinstance(d.get("dual_write"), dict) else None
    names = sorted(i.get("name") for i in items if isinstance(i, dict)) if isinstance(items, list) else None
    return {"mounted": d.get("mounted"), "status": d.get("status"),
            "total_bytes": cap.get("total_bytes"), "file_count": d.get("file_count"), "items": names}


def _case_diag04(ctx: dict) -> CaseResult:
    st, d = _get_json(ctx, "/api/cfgfs")
    if st != 200 or not isinstance(d, dict):
        return _R(Verdict.FAIL, reason=f"GET /api/cfgfs not 200/JSON (status={st})", observed={"status": st})
    if not isinstance(d.get("mounted"), bool):
        return _R(Verdict.FAIL, reason="cfgfs 'mounted' missing or not a bool", observed={"body": d})
    if not d["mounted"]:
        return _R(Verdict.INCONCLUSIVE, reason="cfgfs not mounted (board state; formatting is forbidden)",
                  observed={"mounted": False})
    cap = d.get("capacity")
    if not isinstance(cap, dict) or cap.get("known") is not True:
        return _R(Verdict.FAIL, reason="cfgfs capacity missing or not known", observed={"capacity": cap})
    try:
        if cap["used_bytes"] + cap["free_bytes"] > cap["total_bytes"]:
            return _R(Verdict.FAIL, reason="cfgfs capacity inconsistent: used + free > total",
                      observed={"capacity": cap})
    except (KeyError, TypeError):
        return _R(Verdict.FAIL, reason="cfgfs capacity lacks used/free/total bytes", observed={"capacity": cap})
    dw = d.get("dual_write")
    if not isinstance(dw, dict) or not isinstance(dw.get("items"), list):
        return _R(Verdict.FAIL, reason="cfgfs dual_write.items is not a list", observed={"dual_write": dw})
    stash = ctx.get("_fl07_cfgfs")
    if isinstance(stash, dict):
        a, b = _cfgfs_stable(d), _cfgfs_stable(stash)
        diff = {k: (a[k], b[k]) for k in a if a[k] != b[k]}
        if diff:
            return _R(Verdict.FAIL, reason=f"cfgfs differs from FL-07 on {sorted(diff)}",
                      observed={"diff": diff})
    return _R(Verdict.PASS, observed={"compared_to_fl07": isinstance(stash, dict)})


# ---------------------------------------------------------------------------
# WEB-DIAG-05 -- thermocouple faults
# ---------------------------------------------------------------------------

def _case_diag05(ctx: dict) -> CaseResult:
    st, d = _get_json(ctx, "/api/thermo/faults")
    if st != 200 or not isinstance(d, dict):
        return _R(Verdict.FAIL, reason=f"GET /api/thermo/faults not 200/JSON (status={st})", observed={"status": st})
    chans = d.get("channels")
    problems = []
    if d.get("channel_count") != 3 or not isinstance(chans, list) or len(chans) != 3:
        problems.append(f"channel_count {d.get('channel_count')!r} / channels not 3")
    else:
        for c in chans:
            if not isinstance(c, dict):
                problems.append(f"malformed channel {c!r}")
                continue
            if c.get("state") != "ok":
                problems.append(f"channel {c.get('channel')} state {c.get('state')!r}")
            elif c.get("fault_status") != 0:
                problems.append(f"channel {c.get('channel')} ok with fault_status {c.get('fault_status')!r}")
            elif c.get("stale") is not False:
                problems.append(f"channel {c.get('channel')} stale {c.get('stale')!r}")
    safety = d.get("safety")
    sstate = safety.get("state") if isinstance(safety, dict) else None
    if sstate in ("faulted", "probe_fault", "not_converting"):
        problems.append(f"safety TC state {sstate!r}")
    elif sstate not in ("ok", "no_link"):
        problems.append(f"safety state missing/unknown: {sstate!r}")
    obs = {"safety_state": sstate}
    if sstate == "probe_fault":
        obs["page_finding"] = "page has no probe_fault branch and renders it as ok"
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed=dict(obs, problems=problems))
    if sstate == "no_link":
        return _R(Verdict.INCONCLUSIVE, reason="safety TC state no_link; the three channels read ok", observed=obs)
    return _R(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DIAG-06 -- relay wear rows
# ---------------------------------------------------------------------------

def _case_diag06(ctx: dict) -> CaseResult:
    st, d = _get_json(ctx, "/api/status")
    if st != 200 or not isinstance(d, dict):
        return _R(Verdict.FAIL, reason=f"GET /api/status not 200/JSON (status={st})", observed={"status": st})
    if d.get("io_ready") is False:
        return _R(Verdict.INCONCLUSIVE, reason="io_ready is false", observed={"io_ready": False})
    rl = d.get("relay_life")
    if not isinstance(rl, list) or len(rl) != 5:
        return _R(Verdict.FAIL, reason=f"relay_life is not a 5-entry list (got {len(rl) if isinstance(rl, list) else rl!r})")
    problems = []
    seen = set()
    for e in rl:
        if not isinstance(e, dict):
            problems.append(f"malformed entry {e!r}")
            continue
        seen.add(e.get("relay"))
        if not _is_int(e.get("cycles")) or e["cycles"] < 0:
            problems.append(f"relay {e.get('relay')}: bad cycles")
        if e.get("tier") not in ("none", "warn", "error"):
            problems.append(f"relay {e.get('relay')}: bad tier {e.get('tier')!r}")
        ssr = e.get("type") == "ssr"
        if (e.get("rated") is None) != ssr or (e.get("percent") is None) != ssr:
            problems.append(f"relay {e.get('relay')}: rated/percent null-ness disagrees with type {e.get('type')!r}")
    if seen != {0, 1, 2, 3, 4}:
        problems.append(f"relay indices {sorted(x for x in seen if x is not None)} != 0..4 (K4 = index 4)")
    html, err = _html(ctx, "/diagnostics")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, ("relayWearCard", "relay-reset-btn"))
    if miss:
        problems.append(f"/diagnostics HTML missing {miss}")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed={"problems": problems})
    return _R(Verdict.PASS, observed={"entries": len(rl)})


# ---------------------------------------------------------------------------
# WEB-DIAG-09 -- Danger Mode (owner 2026-10-09: read-only reduction + 409 probe)
# ---------------------------------------------------------------------------

def _case_diag09(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/diagnostics")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    st, d = _get_json(ctx, "/api/diagnostics/danger")
    if st != 200 or not isinstance(d, dict) or not isinstance(d.get("active"), bool):
        return _R(Verdict.FAIL, reason=f"GET /api/diagnostics/danger lacks a bool 'active' (status={st})",
                  observed={"status": st, "body": d})
    if d["active"]:
        return _R(Verdict.INCONCLUSIVE, reason="Danger Mode already active (someone else's session); not touched")
    problems = _missing(html, ('id="dangerAccept"', 'id="dangerEnterBtn"', 'id="dangerLive"', 'id="dangerExitBtn"'))
    problems = [f"missing {m}" for m in problems]
    if not re.search(r'<button[^>]*id="dangerEnterBtn"[^>]*\bdisabled\b', html):
        problems.append("dangerEnterBtn is not disabled by default")
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed={"problems": problems})
    skip = mutating_gate(ctx)
    if skip:
        return _R(Verdict.SKIP, reason=f"refusal probe gated: {skip}")
    # Non-energizing: the firmware rejects /danger/relay with 409 while inactive.
    pst, _pb = _post_json(ctx, "/api/diagnostics/danger/relay", {"relay": "1", "on": "0"})
    if pst == 200:
        return _R(Verdict.FAIL, reason="POST /danger/relay answered 200 while Danger Mode is inactive",
                  observed={"probe_status": pst})
    if pst != 409:
        return _R(Verdict.FAIL, reason=f"POST /danger/relay refusal probe answered {pst}, expected 409",
                  observed={"probe_status": pst})
    return _R(Verdict.PASS, observed={"probe_status": 409, "reduction": "read-only (owner 2026-10-09)"})


# ---------------------------------------------------------------------------
# WEB-DIAG-10 -- lwip_stats / timing / board_temps well-formed
# ---------------------------------------------------------------------------

_TCP_KEYS = ("xmit", "recv", "drop", "chkerr", "lenerr", "memerr", "rterr", "proterr", "opterr", "err")


def _case_diag10(ctx: dict) -> CaseResult:
    # Wire contracts, verified against firmware 2026-10-09:
    #  timing: diagnostics_http.c:1128-1141 (display_flush_us/thermo_read_us
    #    {count,last,min,max,mean}; link_reply_us adds timeouts).
    #  board_temps: board_temps_http.c:40-54 {"esp32_c":f|null,"thermo_cj_c":[f|null..]}.
    #  lwip_stats: diagnostics_http.c:698-722 200 {"ok":true,"tcp":{ten uints}} or 501.
    problems: List[str] = []
    obs: Dict[str, Any] = {}
    st, t = _get_json(ctx, "/api/diagnostics/timing")
    if st != 200 or not isinstance(t, dict):
        problems.append(f"timing not 200/JSON (status={st})")
    else:
        for k in ("display_flush_us", "thermo_read_us", "link_reply_us"):
            o = t.get(k)
            if not isinstance(o, dict) or not all(_is_int(o.get(f)) for f in ("count", "last", "min", "max", "mean")):
                problems.append(f"timing.{k} missing or has non-int fields")
            elif o["count"] > 0 and not (o["min"] <= o["mean"] <= o["max"]):
                problems.append(f"timing.{k} violates min<=mean<=max")
        if not isinstance(t.get("link_reply_us"), dict) or not _is_int(t["link_reply_us"].get("timeouts")):
            problems.append("timing.link_reply_us.timeouts missing or not an int")
    st, b = _get_json(ctx, "/api/board_temps")
    if st != 200 or not isinstance(b, dict):
        problems.append(f"board_temps not 200/JSON (status={st})")
    else:
        e = b.get("esp32_c")
        cj = b.get("thermo_cj_c")
        if "esp32_c" not in b or not (e is None or _is_num(e)):
            problems.append("board_temps.esp32_c missing or non-numeric")
        if not isinstance(cj, list) or not all(v is None or _is_num(v) for v in cj):
            problems.append("board_temps.thermo_cj_c is not a list of numbers/nulls")
    st, lw = _get_json(ctx, "/api/debug/lwip_stats")
    if st == 501:
        obs["lwip"] = "501 not built"
    elif st != 200 or not isinstance(lw, dict) or lw.get("ok") is not True:
        problems.append(f"lwip_stats neither 200 ok:true nor 501 (status={st})")
    else:
        tcp = lw.get("tcp")
        if not isinstance(tcp, dict) or not all(_is_int(tcp.get(k)) for k in _TCP_KEYS):
            problems.append("lwip_stats 200 lacks the ten tcp ints")
        obs["lwip"] = "200"
    if problems:
        return _R(Verdict.FAIL, reason="; ".join(problems), observed=dict(obs, problems=problems))
    return _R(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DIAG-11 -- stale banner (owner 2026-10-09: static presence check)
# ---------------------------------------------------------------------------

def _case_diag11(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/diagnostics")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, ('id="staleBanner"', "staleAge", "function setStale", "failCount"))
    if miss:
        return _R(Verdict.FAIL, reason=f"/diagnostics HTML missing {miss}", observed={"missing": miss})
    return _R(Verdict.PASS, observed={"mode": "static presence (owner 2026-10-09)"})


# ---------------------------------------------------------------------------
# WEB-OTA-02 -- observer of HP-01 (window hp01_running)
# ---------------------------------------------------------------------------

def _ota02_probe(ctx: dict) -> dict:
    st, d = _get_json(ctx, "/api/ota/interlock")
    return {"status": st, "body": d}


def _case_ota02(ctx: dict) -> CaseResult:
    win = (ctx.get("_probe_results") or {}).get("hp01_running")
    if win is None:
        return _R(Verdict.NOT_RUN, reason="HP-01 did not open the hp01_running window in this run")
    res = win.get("WEB-OTA-02")
    if res is None or "error" in res:
        return _R(Verdict.INCONCLUSIVE, reason=f"hp01_running window gave no usable probe result ({res!r})")
    body = res.get("body")
    if res.get("status") != 200 or not isinstance(body, dict) or not isinstance(body.get("ok"), bool):
        return _R(Verdict.FAIL, reason=f"interlock probe malformed (status={res.get('status')})", observed=res)
    if body["ok"] is True or body.get("needs_ack") is True:
        return _R(Verdict.FAIL, reason="interlock allows OTA (ok/needs_ack) during a firing; pickers would show",
                  observed=body)
    if not (isinstance(body.get("reason"), str) and body["reason"].strip()):
        return _R(Verdict.FAIL, reason="interlock blocked with an empty reason", observed=body)
    return _R(Verdict.PASS, observed={"reason": body["reason"]})


# ---------------------------------------------------------------------------
# WEB-OTA-03 / -04 -- DOM path of OT-E01 / OT-E02 (owner 2026-10-09: alias + static)
# ---------------------------------------------------------------------------

def _alias_static(ctx: dict, markers, target: str) -> CaseResult:
    html, err = _html(ctx, "/ota")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, markers)
    if miss:
        return _R(Verdict.FAIL, reason=f"/ota HTML missing {miss}", observed={"missing": miss})
    t = (ctx.get("_results") or {}).get(target)
    if t is None or t.verdict == Verdict.NOT_RUN:
        return _R(Verdict.NOT_RUN, reason=f"{target} did not run in this invocation")
    if t.verdict == Verdict.FAIL:
        return _R(Verdict.FAIL, reason=f"{target} FAILed: {t.reason}", observed={"alias_of": target})
    if t.verdict != Verdict.PASS:
        return _R(Verdict.INCONCLUSIVE, reason=f"{target} was {t.verdict}: {t.reason}", observed={"alias_of": target})
    return _R(Verdict.PASS, observed={"alias_of": target, "static": "present"})


# The single-slot redesign retired the ESP picker/update/rollback controls
# (90b75e27, 02fd73ab); ESP images are staged and installed through the
# Stage card (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP6).
_OTA_STAGE_MARKERS = ('id="stagePicker"', 'id="stageFile"', 'id="stageUploadBtn"', 'id="stageInstallBtn"',
                      "/api/update/stage")
_OTA_RETIRED_MARKERS = ('id="espPicker"', 'id="espFile"', 'id="espUpdateBtn"', 'id="espRollbackBtn"',
                        "/api/ota/esp/rollback")


def _case_ota03(ctx: dict) -> CaseResult:
    # OT-E01 is "a direct ESP push is refused"; the page must no longer offer one.
    html, err = _html(ctx, "/ota")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, _OTA_STAGE_MARKERS)
    if miss:
        return _R(Verdict.FAIL, reason=f"/ota HTML missing Stage/Install card markers {miss}", observed={"missing": miss})
    stale = [m for m in _OTA_RETIRED_MARKERS if m in html]
    if stale:
        return _R(Verdict.FAIL, reason=f"/ota still carries retired ESP push/rollback controls {stale}",
                  observed={"retired_present": stale})
    return _alias_static(ctx, (), "OT-E01")


def _case_ota04(ctx: dict) -> CaseResult:
    # Rollback is retired (OT-E02 reads NOT_RUN by design); static check only.
    html, err = _html(ctx, "/ota")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    stale = [m for m in _OTA_RETIRED_MARKERS if m in html]
    if stale:
        return _R(Verdict.FAIL, reason=f"/ota still carries retired controls {stale}", observed={"retired_present": stale})
    miss = _missing(html, ('id="stageClearBtn"', 'id="stageInstallBtn"'))
    if miss:
        return _R(Verdict.FAIL, reason=f"/ota HTML missing {miss}", observed={"missing": miss})
    return _R(Verdict.PASS, observed={"static": "retired rollback controls absent; Stage card present"})


# ---------------------------------------------------------------------------
# WEB-OTA-05..08
# ---------------------------------------------------------------------------

def _case_ota05(ctx: dict) -> CaseResult:
    st, d = _get_json(ctx, "/api/ota/esp/status")
    if st != 200 or not isinstance(d, dict) or not isinstance(d.get("recovery_mode"), bool):
        return _R(Verdict.FAIL, reason=f"esp/status lacks a bool recovery_mode (status={st})",
                  observed={"status": st, "body": d})
    if d["recovery_mode"]:
        return _R(Verdict.INCONCLUSIVE, reason="recovery_mode true (OT-E11 branch); recoveryExitBtn never pressed")
    html, err = _html(ctx, "/ota")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    if not re.search(r'id="recoveryExitBox"[^>]*display:\s*none', html) or "recoveryExitBtn" not in html:
        return _R(Verdict.FAIL, reason="recoveryExitBox missing or not hidden by default (or recoveryExitBtn absent)")
    return _R(Verdict.PASS, observed={"recovery_mode": False})


def _case_ota06(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/ota")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    miss = _missing(html, tuple(f'id="{i}"' for i in
                                ("picoInfo", "picoPicker", "picoFile", "picoUpdateBtn", "picoRollbackBtn")))
    if miss:
        return _R(Verdict.FAIL, reason=f"/ota HTML missing {miss}", observed={"missing": miss})
    return _R(Verdict.PASS, observed={"posts": 0})


def _case_ota07(ctx: dict) -> CaseResult:
    html, err = _html(ctx, "/ota")
    if html is None:
        return _R(Verdict.INCONCLUSIVE, reason=err or "page unreadable")
    if re.search(r'id="[^"]*bypass[^"]*"', html, re.IGNORECASE):
        return _R(Verdict.FAIL, reason="/ota HTML carries a boot-button bypass banner element")
    st, d = _get_json(ctx, "/api/status")
    if st != 200 or not isinstance(d, dict):
        return _R(Verdict.FAIL, reason=f"GET /api/status not 200/JSON (status={st})", observed={"status": st})
    if d.get("boot_button_bypass_active") not in (None, False):
        return _R(Verdict.FAIL, reason="/api/status reports boot_button_bypass_active true",
                  observed={"boot_button_bypass_active": d.get("boot_button_bypass_active")})
    return _R(Verdict.PASS)


def _case_ota08(ctx: dict) -> CaseResult:
    st, d = _get_json(ctx, "/api/ota/pico/status")
    if st != 200 or not isinstance(d, dict):
        return _R(Verdict.FAIL, reason=f"GET /api/ota/pico/status not 200/JSON (status={st})", observed={"status": st})
    known = d.get("protocol_version_known")
    if known is False:
        return _R(Verdict.INCONCLUSIVE, reason="Pico protocol version not known", observed=d)
    if known is not True or d.get("protocol_compatible") is not True:
        return _R(Verdict.FAIL, reason="Pico protocol not reported compatible (known/compatible must both be true)",
                  observed=d)
    return _R(Verdict.PASS, observed={"protocol_version": d.get("protocol_version")})


_CASE_FUNCS = {
    "WEB-DIAG-02": _case_diag02, "WEB-DIAG-03": _case_diag03, "WEB-DIAG-04": _case_diag04,
    "WEB-DIAG-05": _case_diag05, "WEB-DIAG-06": _case_diag06, "WEB-DIAG-09": _case_diag09,
    "WEB-DIAG-10": _case_diag10, "WEB-DIAG-11": _case_diag11,
    "WEB-OTA-02": _case_ota02, "WEB-OTA-03": _case_ota03, "WEB-OTA-04": _case_ota04,
    "WEB-OTA-05": _case_ota05, "WEB-OTA-06": _case_ota06, "WEB-OTA-07": _case_ota07,
    "WEB-OTA-08": _case_ota08,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
get_case("WEB-OTA-02").window_probe = ("hp01_running", _ota02_probe)
