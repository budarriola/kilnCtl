"""AX suite -- spare-relay aux outputs (docs/SPARE_RELAY_ONOFF_PLAN.md section 12
steps 1-7, docs/BENCH_TEST_SYSTEM_PLAN.md section 3.11). Unit-tested against a
fake board ONLY (``ctx["srv"]`` is the fake); never run against a real board
by the author.

Every case talks to the board through the same MCP functions a human would use
(``control_get_aux_outputs``, ``control_set_aux_output``, ``profile_save_bench_aux_rule``,
``profiles_*``, ``io_read`` via ``srv._io.read().relay(n)`` -- the relay SHADOW,
which proves the ESP path only, not the external supply wiring; CT is N/A
unless a real load is wired through a CT channel and is never judged here).

Confirm gates (all writes): ``AX-C01`` and every heat case SKIP unless
``KILNCTL_AUX_BENCH_CONFIRM=1`` is in the environment (or ``ctx["aux_confirm"]``
is exactly True, the test seam) -- the same env-var opt-in convention as
TP-M01. Heat cases additionally need ``ctx["allow_heat"]``.  A dry run
(``bench_test_run(dry_run=True)``) never calls these functions at all.

Teardown/restore: every heat case stops the firing itself and deletes the
``BENCH_AUX_RULE`` slot it saved; an unconfirmed stop sets ``ctx["_tainted"]``
(and later AX cases SKIP). AX-R01 (runs last) disables relay 4 again if AX-C01
enabled it and verifies ``enabled_mask`` is back to the value AX-C01 recorded.
A failed restore taints the run.

AX-T02 needs a Pico trip. There is no software trip injector; the plan says
"the existing sanctioned bench trip path (never the E-stop jumper)", so this
case is operator-only (``--attended``), same as SP-08, and clears only an
exactly-matched ``trip_mask == 1 << (trip_reason - 1)``.
"""
from __future__ import annotations

import os
import re
import time as _time
from typing import Any, Callable, Dict, List, Optional

from . import judgments as J
from . import operator as OP
from .registry import CaseResult, Verdict, get_case

CONFIRM_ENV = "KILNCTL_AUX_BENCH_CONFIRM"
AUX_RELAY = 4
BENCH_PROFILE_NAME = "BENCH_AUX_RULE"
#: Sampling for the rule/pause cases.
SAMPLE_PERIOD_S = 2.0
RULE_WINDOW_S = 120.0
SKIP_TAINTED = "run tainted by an earlier unconfirmed aux/heat teardown"


def _srv(ctx: dict):
    srv = ctx.get("srv")
    if srv is None:
        from .. import mcp_server as srv  # local import: importable with no board
    return srv


def _confirmed(ctx: dict) -> bool:
    if "aux_confirm" in ctx:
        return ctx["aux_confirm"] is True
    return os.environ.get(CONFIRM_ENV) == "1"


def _gate(ctx: dict, heat: bool) -> Optional[CaseResult]:
    if ctx.get("_tainted"):
        return CaseResult(Verdict.SKIP, reason=SKIP_TAINTED)
    if not _confirmed(ctx):
        return CaseResult(Verdict.SKIP, reason=f"{CONFIRM_ENV}=1 not set (aux cases write board config)")
    if heat and ctx.get("allow_heat") is not True:
        return CaseResult(Verdict.SKIP, reason="allow_heat=False")
    return None


# -- parsing ---------------------------------------------------------------

_ENTRY_RE = re.compile(
    r"relay (\d+): (ENABLED|disabled)( CONFLICTED)?, tc_zone=(none|-?\d+), hyst_c=([^,]+), "
    r"min_on_s=(\d+), min_off_s=(\d+)")


def _to_float(v: str) -> Optional[float]:
    try:
        return float(v)
    except ValueError:
        return None


def parse_aux_outputs(text: str) -> Dict[str, Any]:
    """Parse ``control_get_aux_outputs`` text into
    ``{"enabled_mask": int|None, "relays": {n: {...}}}``; ``{}`` on error text."""
    if not isinstance(text, str) or text.startswith(("error", "refused")):
        return {}
    m = re.search(r"enabled_mask=(\d+)", text)
    relays: Dict[int, dict] = {}
    for e in _ENTRY_RE.finditer(text):
        relays[int(e.group(1))] = {
            "enabled": e.group(2) == "ENABLED", "conflicted": bool(e.group(3)),
            "tc_zone": None if e.group(4) == "none" else int(e.group(4)),
            "hyst_c": _to_float(e.group(5)),
            "min_on_s": int(e.group(6)), "min_off_s": int(e.group(7)),
        }
    return {"enabled_mask": int(m.group(1)) if m else None, "relays": relays}


def _aux_state(ctx: dict) -> Dict[str, Any]:
    return parse_aux_outputs(_srv(ctx).control_get_aux_outputs())


def _relay_on(ctx: dict, relay: int = AUX_RELAY) -> Optional[bool]:
    fn = ctx.get("aux_relay_fn")
    try:
        if fn is not None:
            return fn(relay)
        return bool(_srv(ctx)._io.read().relay(relay))
    except Exception:  # noqa: BLE001 - unreadable is "unknown", never "off"
        return None


def _ambient_c(ctx: dict) -> Optional[float]:
    fn = ctx.get("aux_ambient_fn")
    try:
        if fn is not None:
            return float(fn())
        # devices_thermo.describe(): "CH0: 21.50 C (CJ 23.00 C)" or "CH0: invalid".
        m = re.search(r"CH\d+:\s*(-?\d+(?:\.\d+)?)\s*C\b", _srv(ctx).thermo_read(0))
        return float(m.group(1)) if m else None
    except Exception:  # noqa: BLE001
        return None


_EXEC_STATE_NAMES = {0: "idle", 1: "running", 2: "paused", 3: "done", 4: "faulted"}  # profile_exec_state_t
_EXEC_RE = re.compile(r"state=(\w+)\s+profile=#(-?\d+)")


def _exec(ctx: dict) -> Optional["tuple[str, int]"]:
    """(state_name_lower, profile_id) of the executor, or None if unreadable."""
    try:
        fn = ctx.get("aux_exec_fn")
        text = fn() if fn is not None else _srv(ctx).profiles_get_exec_status()
        if isinstance(text, tuple):
            return text
        m = _EXEC_RE.search(text)
        if not m:
            return None
        tok = m.group(1).lower()
        if tok.isdigit():
            # The MCP text prints the raw profile_exec_state_t int ("state=1"), not its name.
            tok = _EXEC_STATE_NAMES.get(int(tok), "")
        if tok not in _EXEC_STATE_NAMES.values():
            return None  # unparseable state -> unreadable, which every caller treats as a FAIL
        return (tok, int(m.group(2)))
    except Exception:  # noqa: BLE001
        return None


def _slot_exists(ctx: dict) -> Optional[bool]:
    try:
        out = _srv(ctx).profiles_list()
        if isinstance(out, str) and out.lstrip().lower().startswith(("error", "refused")):
            return None
        return BENCH_PROFILE_NAME in out
    except Exception:  # noqa: BLE001
        return None


def _start_and_confirm(ctx: dict, pid: int) -> Optional[CaseResult]:
    out = _srv(ctx).profiles_start(pid)
    if not _is_ok(out):
        return CaseResult(Verdict.FAIL, reason=f"profiles_start: {str(out)[:120]}")
    ex = _exec(ctx)
    if ex is None or ex != ("running", pid):
        return CaseResult(Verdict.FAIL, reason=f"executor not running profile {pid} after start (read {ex})")
    return None


def _sleep(ctx: dict, s: float) -> None:
    (ctx.get("sleep_fn") or _time.sleep)(s)


def _is_ok(text: str) -> bool:
    return isinstance(text, str) and text.startswith("ok")


# -- judges (pure) ----------------------------------------------------------

def judge_aux_configured(entry: Optional[dict], enabled_mask: Optional[int]) -> CaseResult:
    if not entry:
        return CaseResult(Verdict.FAIL, reason=f"relay {AUX_RELAY} missing from control_get_aux_outputs")
    if not entry["enabled"] or entry["tc_zone"] != 0:
        return CaseResult(Verdict.FAIL, reason=f"read-back does not show relay {AUX_RELAY} enabled with tc_zone 0",
                          observed=entry)
    if entry["conflicted"]:
        return CaseResult(Verdict.FAIL, reason="relay 4 aux binding reads CONFLICTED", observed=entry)
    return CaseResult(Verdict.PASS, observed={"entry": entry, "enabled_mask": enabled_mask})


def judge_rule_samples(samples: List["tuple[float, Optional[bool]]"], min_on_s: int, min_off_s: int,
                       tol_s: float = SAMPLE_PERIOD_S) -> CaseResult:
    """``samples`` = (t_s, relay4_shadow). PASS needs at least one ON sample
    (the rule fired) and no completed interval shorter than min_on/min_off
    (minus the sampling period). One transition only is stated plainly."""
    if any(v is None for _, v in samples) or not samples:
        return CaseResult(Verdict.INCONCLUSIVE, reason="relay shadow unreadable for some samples",
                          observed={"samples": samples})
    if not any(v for _, v in samples):
        return CaseResult(Verdict.FAIL, reason="relay 4 never read ON although the rule should fire at segment start",
                          observed={"samples": samples})
    runs: List["tuple[bool, float]"] = []
    start_t, cur = samples[0]
    for t, v in samples[1:]:
        if v != cur:
            runs.append((cur, t - start_t))
            start_t, cur = t, v
    transitions = len(runs)
    # The first run is cut off by the sample start, not a firmware switch: skip it.
    bad = [(("ON" if s else "OFF"), d) for s, d in runs[1:]
           if d + tol_s < (min_on_s if s else min_off_s)]
    if bad:
        return CaseResult(Verdict.FAIL, reason=f"min on/off violated: {bad}", observed={"runs": runs})
    if transitions < 2:
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason=f"only {transitions} transition(s) observed; need >= 2 to exercise min on/off",
                          observed={"transitions": transitions, "runs": runs})
    return CaseResult(Verdict.PASS, observed={"transitions": transitions, "runs": runs})


def judge_conflict_refusal(text: str, mask_before: Optional[int], mask_after: Optional[int]) -> CaseResult:
    refused = isinstance(text, str) and text.startswith("refused") and (
        "400" in text or "relay_mask" in text or "conflict" in text.lower())
    if not refused:
        return CaseResult(Verdict.FAIL, reason=f"enabling aux on a zone-owned relay was not refused: {text[:120]!r}")
    if mask_before is None or mask_after != mask_before:
        return CaseResult(Verdict.FAIL, reason=f"enabled_mask changed {mask_before} -> {mask_after} despite refusal")
    return CaseResult(Verdict.PASS, observed={"refusal": text[:160]})


# -- shared run helper -------------------------------------------------------

def _save_rule(ctx: dict, threshold_delta: float, target_delta: float = 10.0) -> "tuple[Optional[int], Optional[CaseResult]]":
    """(pid, None) on success else (None, CaseResult to return)."""
    if _slot_exists(ctx) is not False:
        return None, CaseResult(Verdict.SKIP, reason=f"profile {BENCH_PROFILE_NAME} already exists (or unreadable); "
                                "not overwriting it")
    amb = _ambient_c(ctx)
    if amb is None:
        return None, CaseResult(Verdict.INCONCLUSIVE, reason="could not read a valid ambient zone-0 temperature")
    out = _srv(ctx).profile_save_bench_aux_rule(
        target_c=amb + target_delta, threshold_c=amb + threshold_delta, temp_cmp="below", confirm=True)
    m = re.search(r"profile id (\d+)", out) if isinstance(out, str) else None
    if not _is_ok(out) or not m:
        return None, CaseResult(Verdict.FAIL, reason=f"profile_save_bench_aux_rule: {str(out)[:160]}")
    pid = int(m.group(1))
    ctx.setdefault("_aux_profile_ids", []).append(pid)
    return pid, None


def _teardown(ctx: dict, pid: Optional[int]) -> str:
    """Stop the firing and delete the saved slot. Returns "" or a problem."""
    srv = _srv(ctx)
    problems = []
    ex = _exec(ctx)
    # Stop only our own firing; unreadable status -> stop (fail safe, we may have started it).
    if pid is not None and (ex is None or (ex[0] != "idle" and ex[1] == pid)):
        try:
            srv.profiles_stop()
        except Exception as exc:  # noqa: BLE001
            problems.append(f"profiles_stop raised {type(exc).__name__}")
    if ctx.get("aux_idle_fn") is not None:
        idle = ctx["aux_idle_fn"]()
    else:
        try:
            idle = getattr(srv._profiles.get_exec_status(), "state_name", None) == "idle"
        except Exception:  # noqa: BLE001
            idle = False
    if not idle:
        problems.append("executor not confirmed idle after stop")
    if pid is not None:
        try:
            out = srv.profiles_delete(pid)
            if isinstance(out, str) and out.startswith(("error", "refused")):
                problems.append(f"profiles_delete({pid}): {out[:80]}")
            else:
                ctx["_aux_profile_ids"] = [p for p in ctx.get("_aux_profile_ids", []) if p != pid]
        except Exception as exc:  # noqa: BLE001
            problems.append(f"profiles_delete raised {type(exc).__name__}")
    return "; ".join(problems)


def _finish(ctx: dict, pid: Optional[int], result: CaseResult) -> CaseResult:
    problem = _teardown(ctx, pid)
    if problem:
        ctx["_tainted"] = True
        return CaseResult(Verdict.FAIL, reason=f"teardown not confirmed ({problem}) -- run tainted",
                          observed=result.observed)
    return result


def _preflight(ctx: dict) -> Optional[CaseResult]:
    """Plan step 1 + the bound aux output: executor idle, relay 4 an enabled aux."""
    st = _aux_state(ctx)
    ent = st.get("relays", {}).get(AUX_RELAY)
    if not ent or not ent["enabled"]:
        return CaseResult(Verdict.SKIP, reason="relay 4 is not an enabled aux output (AX-C01 did not run/pass)")
    return None


# -- cases -------------------------------------------------------------------

def _fw_post_aux(ctx: dict, relay: int, enabled: bool, tc_zone: Optional[int]) -> "tuple[Optional[int], bool, str]":
    """POST /api/aux_outputs straight through the HTTP client (no MCP precheck).
    Returns (http_status_or_None, board_said_ok, refusal_detail). ``ctx["aux_post_fn"]`` is the
    test seam and may return a 2-tuple (detail then "") or a 3-tuple."""
    fn = ctx.get("aux_post_fn")
    if fn is not None:
        r = fn(relay, enabled, tc_zone)
        return (r[0], r[1], r[2] if len(r) > 2 else "")
    from .. import aux_http_client as ahc
    from ..mcp_server_aux import _resolve_host
    try:
        return 200, bool(ahc.post_aux_output(_resolve_host(ctx.get("host")), relay, enabled, tc_zone)), ""
    except ahc.AuxHttpError as exc:
        return exc.status, False, f"{exc} {exc.detail}"


def _restore(ctx: dict) -> List[str]:
    """Put relay 4's full aux entry and enabled_mask back to what AX-C01 recorded.
    Idempotent; returns problems ([] on success or when nothing is dirty)."""
    orig = ctx.get("_aux_orig")
    if orig is None or not ctx.get("_aux_dirty"):
        return []
    srv = _srv(ctx)
    problems: List[str] = []
    o = orig["relay4"]
    if o:
        out = srv.control_set_aux_output(
            relay=AUX_RELAY, enabled=o["enabled"], tc_zone=-1 if o["tc_zone"] is None else o["tc_zone"],
            hyst_c=o.get("hyst_c"), min_on_s=o["min_on_s"], min_off_s=o["min_off_s"], confirm=True)
    else:
        out = srv.control_set_aux_output(relay=AUX_RELAY, enabled=False, confirm=True)
    if not _is_ok(out):
        problems.append(f"restore relay 4: {str(out)[:100]}")
    now = _aux_state(ctx)
    cur = now.get("relays", {}).get(AUX_RELAY, {})
    for k in ("enabled", "tc_zone", "hyst_c", "min_on_s", "min_off_s"):
        if o and cur.get(k) != o.get(k):
            problems.append(f"relay 4 {k} {cur.get(k)!r} != original {o.get(k)!r}")
    if now.get("enabled_mask") != orig["enabled_mask"]:
        problems.append(f"enabled_mask {now.get('enabled_mask')} != original {orig['enabled_mask']}")
    if not problems:
        ctx["_aux_dirty"] = False
    return problems


def _delete_bench_slot(ctx: dict, pid: int) -> Optional[str]:
    """Delete slot ``pid`` only if it still holds BENCH_AUX_RULE; always drop it from the tracked list."""
    srv = _srv(ctx)
    ctx["_aux_profile_ids"] = [p for p in ctx.get("_aux_profile_ids", []) if p != pid]
    try:
        info = srv.profiles_get(pid)
    except Exception as exc:  # noqa: BLE001
        return f"profiles_get({pid}): {exc}"
    if not isinstance(info, str) or info.lstrip().lower().startswith(("error", "refused")):
        return f"profiles_get({pid}): {str(info)[:80]}"
    if info.lstrip().lower().startswith("no such"):
        return None  # already gone
    if BENCH_PROFILE_NAME not in info:
        return f"slot {pid} no longer holds {BENCH_PROFILE_NAME}; not deleted"
    out = srv.profiles_delete(pid)
    if isinstance(out, str) and out.startswith(("error", "refused")):
        return f"profiles_delete({pid}): {out[:80]}"
    return None


def aux_teardown_hook(ctx: dict) -> None:
    """Runner teardown: restore relay 4 if AX-C01 mutated it and AX-R01 never ran/finished."""
    if ctx.get("_aux_dirty") and _restore(ctx):
        ctx["_tainted"] = True
    # Saved BENCH_AUX_RULE slots must not outlive an aborted run (later runs would skip at _slot_exists).
    errors = []
    for pid in list(ctx.get("_aux_profile_ids", [])):
        err = _delete_bench_slot(ctx, pid)
        if err:
            errors.append(err)
    if errors:
        ctx["_tainted"] = True
        raise RuntimeError("; ".join(errors))


def _case_ax_c01(ctx: dict) -> CaseResult:
    skip = _gate(ctx, heat=False)
    if skip:
        return skip
    srv = _srv(ctx)
    if _slot_exists(ctx) is not False:
        ctx["_tainted"] = True  # later aux cases must not run either
        return CaseResult(Verdict.SKIP, reason=f"profile {BENCH_PROFILE_NAME} already exists (or unreadable); "
                          "suite skipped rather than overwrite it")
    before = _aux_state(ctx)
    if not before:
        return CaseResult(Verdict.INCONCLUSIVE, reason="control_get_aux_outputs unreadable")
    ctx["_aux_orig"] = {"enabled_mask": before["enabled_mask"],
                        "relay4": dict(before["relays"].get(AUX_RELAY, {}))}
    ctx["_aux_dirty"] = True
    ctx.setdefault("teardown_hooks", [])
    if aux_teardown_hook not in ctx["teardown_hooks"]:
        ctx["teardown_hooks"].append(aux_teardown_hook)
    out = srv.control_set_aux_output(relay=AUX_RELAY, enabled=True, tc_zone=0, confirm=True)
    if not _is_ok(out):
        return CaseResult(Verdict.FAIL, reason=f"control_set_aux_output: {str(out)[:160]}")
    after = _aux_state(ctx)
    return judge_aux_configured(after.get("relays", {}).get(AUX_RELAY), after.get("enabled_mask"))


def _case_ax_c02(ctx: dict) -> CaseResult:
    """Plan step 5a: enabling aux on a zone-owned relay (relay 1) is refused. Two
    sub-checks: the MCP precheck refusal, and the FIRMWARE's own 400 via a direct POST."""
    skip = _gate(ctx, heat=False)
    if skip:
        return skip
    before = _aux_state(ctx)
    out = _srv(ctx).control_set_aux_output(relay=1, enabled=True, tc_zone=0, confirm=True)
    pre = judge_conflict_refusal(out, before.get("enabled_mask"), _aux_state(ctx).get("enabled_mask"))
    status, fw_ok, detail = _fw_post_aux(ctx, 1, True, 0)
    after = _aux_state(ctx)
    changed = after.get("relays", {}).get(1, {}).get("enabled") and not before.get("relays", {}).get(1, {}).get("enabled")
    if changed or fw_ok or pre.verdict == Verdict.FAIL and "changed" in pre.reason:
        undo = _srv(ctx).control_set_aux_output(relay=1, enabled=False, confirm=True)
        if not _is_ok(undo) and _aux_state(ctx).get("relays", {}).get(1, {}).get("enabled"):
            ctx["_tainted"] = True
            return CaseResult(Verdict.FAIL, reason="relay 1 aux write was accepted and could not be disabled -- run tainted")
    obs = {"precheck": pre.verdict, "firmware_status": status, "firmware_ok": fw_ok, "firmware_detail": detail}
    # aux_outputs_http_core.c: enabling aux on a zone-claimed relay is a CONFLICT (409,
    # "claimed by a zone relay_mask"); 400 is reserved for field-range validation.
    if status != 409 or "zone relay_mask" not in detail:
        return CaseResult(Verdict.FAIL, reason=f"firmware answered {status} (ok={fw_ok}, detail={detail[:80]!r}) to aux on a "
                          "zone-owned relay, expected 409 'claimed by a zone relay_mask'", observed=obs)
    if after.get("enabled_mask") != before.get("enabled_mask"):
        return CaseResult(Verdict.FAIL, reason="enabled_mask changed despite firmware 409", observed=obs)
    if pre.verdict != Verdict.PASS:
        return CaseResult(Verdict.FAIL, reason=f"MCP precheck sub-check: {pre.reason}", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _default_zone_mask_fns(ctx: dict):
    """Build (post_fn, restore_fn) over control_set_zone_relay_mask: pick the first zone GET
    /api/zones reports, post its current mask | relay 4's bit, and restore the original on an
    accepted write. Returns None when the zones cannot be read."""
    from .. import zones_http_client as zhc
    from ..mcp_server_aux import _resolve_host
    srv = _srv(ctx)
    try:
        host = _resolve_host(ctx.get("host"))
        zones = zhc.get_zones(host).get("zones") or []
        z = next(z for z in zones if "index" in z and isinstance(z.get("relay_mask"), int))
    except Exception:  # noqa: BLE001
        return None
    zone, orig = z["index"], z["relay_mask"]
    bad = orig | (1 << (AUX_RELAY - 1))

    def post():
        out = str(srv.control_set_zone_relay_mask(zone=zone, relay_mask=bad, confirm=True))
        m = re.search(r"refused by firmware \(HTTP (\d+)\)", out)
        if m:
            return int(m.group(1)), out
        return (200 if out.startswith("ok") else None), out

    def restore():
        return str(srv.control_set_zone_relay_mask(zone=zone, relay_mask=orig, confirm=True)).startswith("ok")

    return post, restore


def _case_ax_c03(ctx: dict) -> CaseResult:
    """Plan step 5b: a zone relay_mask containing relay 4 is refused (400).
    Uses the narrow control_set_zone_relay_mask tool on the first zone; the write can be
    injected via ``ctx["aux_zone_mask_post_fn"]() -> (status, body)``. An accepted write is
    undone (the original mask is restored) and always taints the run. A status other than
    400/2xx (tool precheck refusal, transport error) is INCONCLUSIVE, never PASS."""
    skip = _gate(ctx, heat=False)
    if skip:
        return skip
    fn = ctx.get("aux_zone_mask_post_fn")
    if fn is None:
        pair = _default_zone_mask_fns(ctx)
        if pair is None:
            return CaseResult(Verdict.SKIP, reason="could not read a zone relay_mask from GET /api/zones")
        fn = pair[0]
        ctx.setdefault("aux_zone_mask_restore_fn", pair[1])
    status, body = fn()
    if status == 400:
        return CaseResult(Verdict.PASS, observed={"status": status})
    restored = None
    if status is not None and 200 <= status < 300:
        ctx["_tainted"] = True
        rfn = ctx.get("aux_zone_mask_restore_fn")
        try:
            restored = bool(rfn()) if rfn is not None else False
        except Exception:  # noqa: BLE001
            restored = False
    else:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"no firmware verdict (status {status}): {str(body)[:120]}",
                          observed={"status": status})
    return CaseResult(Verdict.FAIL, reason=f"zone relay_mask containing relay 4 answered {status}, expected 400"
                      + ("" if restored is None else f"; original mask restore {'ok' if restored else 'NOT confirmed'}"
                         " -- run tainted"), observed={"status": status, "restored": restored})


def _case_ax_t01(ctx: dict) -> CaseResult:
    """Plan steps 3/4: the rule toggles relay 4 (io_read shadow), honouring min on/off."""
    skip = _gate(ctx, heat=True) or _preflight(ctx)
    if skip:
        return skip
    ent = _aux_state(ctx)["relays"][AUX_RELAY]
    pid, err = _save_rule(ctx, threshold_delta=4.0)
    if pid is None:
        return err
    samples: List["tuple[float, Optional[bool]]"] = []
    try:
        pre = _relay_on(ctx)
        if pre is not False:
            return _finish(ctx, pid, CaseResult(Verdict.INCONCLUSIVE,
                                                reason=f"relay 4 read {pre} before profiles_start; need OFF"))
        bad = _start_and_confirm(ctx, pid)
        if bad:
            return _finish(ctx, pid, bad)
        t = 0.0
        while t <= ctx.get("aux_window_s", RULE_WINDOW_S):
            samples.append((t, _relay_on(ctx)))
            _sleep(ctx, SAMPLE_PERIOD_S)
            t += SAMPLE_PERIOD_S
        result = judge_rule_samples(samples, ent["min_on_s"], ent["min_off_s"])
    except Exception as exc:  # noqa: BLE001
        result = CaseResult(Verdict.FAIL, reason=f"raised {type(exc).__name__}: {exc}")
    return _finish(ctx, pid, result)


def _case_ax_k01(ctx: dict) -> CaseResult:
    """Plan step 7: relay 4 still follows its rule while PAUSED (no heat granted)."""
    skip = _gate(ctx, heat=True) or _preflight(ctx)
    if skip:
        return skip
    pid, err = _save_rule(ctx, threshold_delta=25.0)  # stays below threshold: R4 ON throughout
    if pid is None:
        return err
    srv = _srv(ctx)
    try:
        bad = _start_and_confirm(ctx, pid)
        if bad:
            return _finish(ctx, pid, bad)
        _sleep(ctx, SAMPLE_PERIOD_S)
        running_on = _relay_on(ctx)
        if running_on is not True:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"relay 4 read {running_on} before pausing; nothing to compare")
        else:
            pout = srv.profiles_pause()
            if not _is_ok(pout) or (_exec(ctx) or ("", -1))[0] != "paused":
                return _finish(ctx, pid, CaseResult(
                    Verdict.FAIL, reason=f"profiles_pause did not pause the run ({str(pout)[:80]}; state {_exec(ctx)})"))
            _sleep(ctx, SAMPLE_PERIOD_S)
            paused_on = _relay_on(ctx)
            try:
                srv.profiles_resume()
            except Exception:  # noqa: BLE001 - teardown stops it anyway
                pass
            note = "ESP path only; proves nothing about the external supply wiring"
            if paused_on is True:
                result = CaseResult(Verdict.PASS, reason=note, observed={"paused_relay4": True})
            else:
                result = CaseResult(
                    Verdict.FAIL, reason=f"relay 4 read {paused_on} while PAUSED; the rule should hold it ON (finding, "
                    "report it -- plan section 6 item 4)", observed={"paused_relay4": paused_on})
    except Exception as exc:  # noqa: BLE001
        result = CaseResult(Verdict.FAIL, reason=f"raised {type(exc).__name__}: {exc}")
    return _finish(ctx, pid, result)


def _case_ax_t02(ctx: dict) -> CaseResult:
    """Plan step 6: a Pico trip drops relay 4. Operator-only (no trip injector)."""
    skip = OP.require_attended(ctx) or _gate(ctx, heat=True) or _preflight(ctx)
    if skip:
        return skip
    pid, err = _save_rule(ctx, threshold_delta=25.0)
    if pid is None:
        return err
    srv = _srv(ctx)
    try:
        bad = _start_and_confirm(ctx, pid)
        if bad:
            return _finish(ctx, pid, bad)
        _sleep(ctx, SAMPLE_PERIOD_S)
        if _relay_on(ctx) is not True:
            result = CaseResult(Verdict.INCONCLUSIVE, reason="relay 4 not ON before the trip; cannot show it drops")
        elif not OP.ask_operator(ctx, "Relay 4 is ON. Cause the sanctioned bench Pico trip now (NOT the E-stop "
                                      "jumper), then confirm.", timeout_s=180.0):
            result = CaseResult(Verdict.FAIL, reason="operator did not confirm the trip was caused")
        else:
            _sleep(ctx, SAMPLE_PERIOD_S)
            after = _relay_on(ctx)
            diag = srv.safety_get_diag()
            reason = J.parse_trip_reason(diag)
            mask = J.parse_trip_mask(diag)
            cleared = None
            if reason and mask == J.safety_trip_mask_for_reason(reason):
                srv.safety_clear_trip()
                r2 = J.parse_trip_reason(srv.safety_get_diag())
                cleared = None if r2 is None else r2 == 0
            obs = {"relay4_after_trip": after, "trip_reason": reason, "trip_mask": mask, "cleared": cleared}
            if not reason:
                result = CaseResult(Verdict.FAIL, reason="no trip latched after the operator's action", observed=obs)
            elif after is not False:
                result = CaseResult(Verdict.FAIL, reason=f"relay 4 read {after} after the trip, expected OFF", observed=obs)
            elif cleared is not True:
                ctx["_tainted"] = True
                result = CaseResult(Verdict.FAIL, reason="relay 4 dropped, but the trip was not cleanly cleared -- run tainted",
                                    observed=obs)
            else:
                result = CaseResult(Verdict.PASS, observed=obs)
    except Exception as exc:  # noqa: BLE001
        result = CaseResult(Verdict.FAIL, reason=f"raised {type(exc).__name__}: {exc}")
    return _finish(ctx, pid, result)


def _case_ax_r01(ctx: dict) -> CaseResult:
    """Restore: relay 4's FULL aux entry back to what AX-C01 found, no stray BENCH_AUX_RULE slot."""
    orig = ctx.get("_aux_orig")
    if orig is None:
        return CaseResult(Verdict.SKIP, reason="AX-C01 did not change anything")
    srv = _srv(ctx)
    problems = []
    for pid in list(ctx.get("_aux_profile_ids", [])):
        err = _delete_bench_slot(ctx, pid)
        if err:
            problems.append(err)
    problems += _restore(ctx)
    if problems:
        ctx["_tainted"] = True
        return CaseResult(Verdict.FAIL, reason="; ".join(problems) + " -- run tainted")
    return CaseResult(Verdict.PASS, observed={"enabled_mask": _aux_state(ctx).get("enabled_mask")})


_CASE_FUNCS: Dict[str, Callable[[dict], CaseResult]] = {
    "AX-C01": _case_ax_c01, "AX-C02": _case_ax_c02, "AX-C03": _case_ax_c03,
    "AX-T01": _case_ax_t01, "AX-K01": _case_ax_k01, "AX-T02": _case_ax_t02,
    "AX-R01": _case_ax_r01,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
