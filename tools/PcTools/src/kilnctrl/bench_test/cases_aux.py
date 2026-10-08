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
    if heat and not ctx.get("allow_heat"):
        return CaseResult(Verdict.SKIP, reason="allow_heat=False")
    return None


# -- parsing ---------------------------------------------------------------

_ENTRY_RE = re.compile(
    r"relay (\d+): (ENABLED|disabled)( CONFLICTED)?, tc_zone=(none|-?\d+), hyst_c=([^,]+), "
    r"min_on_s=(\d+), min_off_s=(\d+)")


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
        m = re.search(r"(-?\d+(?:\.\d+)?)", _srv(ctx).thermo_read(0))
        return float(m.group(1)) if m else None
    except Exception:  # noqa: BLE001
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
    note = "" if transitions >= 2 else " (only one transition observed; min on/off not exercised)"
    return CaseResult(Verdict.PASS, reason=f"rule toggled relay 4{note}" if note else "",
                      observed={"transitions": transitions, "runs": runs})


def judge_conflict_refusal(text: str, mask_before: Optional[int], mask_after: Optional[int]) -> CaseResult:
    refused = isinstance(text, str) and text.startswith("refused") and (
        "400" in text or "relay_mask" in text or "conflict" in text.lower())
    if not refused:
        return CaseResult(Verdict.FAIL, reason=f"enabling aux on a zone-owned relay was not refused: {text[:120]!r}")
    if mask_before is None or mask_after != mask_before:
        return CaseResult(Verdict.FAIL, reason=f"enabled_mask changed {mask_before} -> {mask_after} despite refusal")
    return CaseResult(Verdict.PASS, observed={"refusal": text[:160]})


# -- shared run helper -------------------------------------------------------

def _save_rule(ctx: dict, threshold_delta: float, target_delta: float = 10.0) -> "tuple[Optional[int], str]":
    amb = _ambient_c(ctx)
    if amb is None:
        return None, "could not read ambient zone-0 temperature"
    out = _srv(ctx).profile_save_bench_aux_rule(
        target_c=amb + target_delta, threshold_c=amb + threshold_delta, temp_cmp="below", confirm=True)
    m = re.search(r"profile id (\d+)", out) if isinstance(out, str) else None
    if not _is_ok(out) or not m:
        return None, f"profile_save_bench_aux_rule: {str(out)[:160]}"
    pid = int(m.group(1))
    ctx.setdefault("_aux_profile_ids", []).append(pid)
    return pid, ""


def _teardown(ctx: dict, pid: Optional[int]) -> str:
    """Stop the firing and delete the saved slot. Returns "" or a problem."""
    srv = _srv(ctx)
    problems = []
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

def _case_ax_c01(ctx: dict) -> CaseResult:
    skip = _gate(ctx, heat=False)
    if skip:
        return skip
    srv = _srv(ctx)
    before = _aux_state(ctx)
    if not before:
        return CaseResult(Verdict.INCONCLUSIVE, reason="control_get_aux_outputs unreadable")
    ctx["_aux_orig"] = {"enabled_mask": before["enabled_mask"],
                        "relay4": dict(before["relays"].get(AUX_RELAY, {}))}
    out = srv.control_set_aux_output(relay=AUX_RELAY, enabled=True, tc_zone=0, confirm=True)
    if not _is_ok(out):
        return CaseResult(Verdict.FAIL, reason=f"control_set_aux_output: {str(out)[:160]}")
    after = _aux_state(ctx)
    return judge_aux_configured(after.get("relays", {}).get(AUX_RELAY), after.get("enabled_mask"))


def _case_ax_c02(ctx: dict) -> CaseResult:
    """Plan step 5a: enabling aux on a zone-owned relay (relay 1) is refused."""
    skip = _gate(ctx, heat=False)
    if skip:
        return skip
    before = _aux_state(ctx)
    out = _srv(ctx).control_set_aux_output(relay=1, enabled=True, tc_zone=0, confirm=True)
    after = _aux_state(ctx)
    res = judge_conflict_refusal(out, before.get("enabled_mask"), after.get("enabled_mask"))
    if res.verdict == Verdict.FAIL and after.get("relays", {}).get(1, {}).get("enabled") and \
            not before.get("relays", {}).get(1, {}).get("enabled"):
        undo = _srv(ctx).control_set_aux_output(relay=1, enabled=False, confirm=True)
        if not _is_ok(undo):
            ctx["_tainted"] = True
            return CaseResult(Verdict.FAIL, reason=res.reason + "; ALSO could not disable relay 1 again -- run tainted")
    return res


def _case_ax_c03(ctx: dict) -> CaseResult:
    """Plan step 5b: a zone relay_mask containing relay 4 is refused (400).
    No narrow relay_mask writer exists, so the write is injected via
    ``ctx["aux_zone_mask_post_fn"]() -> (status, body)`` and SKIPs without it."""
    skip = _gate(ctx, heat=False)
    if skip:
        return skip
    fn = ctx.get("aux_zone_mask_post_fn")
    if fn is None:
        return CaseResult(Verdict.SKIP, reason="no zone relay_mask writer injected (no narrow tool exists)")
    status, _body = fn()
    if status == 400:
        return CaseResult(Verdict.PASS, observed={"status": status})
    return CaseResult(Verdict.FAIL, reason=f"zone relay_mask containing relay 4 answered {status}, expected 400")


def _case_ax_t01(ctx: dict) -> CaseResult:
    """Plan steps 3/4: the rule toggles relay 4 (io_read shadow), honouring min on/off."""
    skip = _gate(ctx, heat=True) or _preflight(ctx)
    if skip:
        return skip
    ent = _aux_state(ctx)["relays"][AUX_RELAY]
    pid, err = _save_rule(ctx, threshold_delta=4.0)
    if pid is None:
        return CaseResult(Verdict.FAIL, reason=err)
    samples: List["tuple[float, Optional[bool]]"] = []
    try:
        started = _srv(ctx).profiles_start(pid)
        if isinstance(started, str) and started.startswith(("error", "refused")):
            return _finish(ctx, pid, CaseResult(Verdict.FAIL, reason=f"profiles_start: {started[:120]}"))
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
        return CaseResult(Verdict.FAIL, reason=err)
    srv = _srv(ctx)
    try:
        srv.profiles_start(pid)
        _sleep(ctx, SAMPLE_PERIOD_S)
        running_on = _relay_on(ctx)
        if running_on is not True:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"relay 4 read {running_on} before pausing; nothing to compare")
        else:
            srv.profiles_pause()
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
        return CaseResult(Verdict.FAIL, reason=err)
    srv = _srv(ctx)
    try:
        srv.profiles_start(pid)
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
    """Restore: relay 4 back to what AX-C01 found, no stray BENCH_AUX_RULE slot."""
    orig = ctx.get("_aux_orig")
    if orig is None:
        return CaseResult(Verdict.SKIP, reason="AX-C01 did not change anything")
    srv = _srv(ctx)
    problems = []
    for pid in list(ctx.get("_aux_profile_ids", [])):
        out = srv.profiles_delete(pid)
        if isinstance(out, str) and out.startswith(("error", "refused")):
            problems.append(f"profiles_delete({pid}): {out[:80]}")
    if not orig["relay4"].get("enabled"):
        out = srv.control_set_aux_output(relay=AUX_RELAY, enabled=False, confirm=True)
        if not _is_ok(out):
            problems.append(f"disable relay 4: {str(out)[:100]}")
    now = _aux_state(ctx)
    if now.get("enabled_mask") != orig["enabled_mask"]:
        problems.append(f"enabled_mask {now.get('enabled_mask')} != original {orig['enabled_mask']}")
    if problems:
        ctx["_tainted"] = True
        return CaseResult(Verdict.FAIL, reason="; ".join(problems) + " -- run tainted")
    return CaseResult(Verdict.PASS, observed={"enabled_mask": now.get("enabled_mask")})


_CASE_FUNCS: Dict[str, Callable[[dict], CaseResult]] = {
    "AX-C01": _case_ax_c01, "AX-C02": _case_ax_c02, "AX-C03": _case_ax_c03,
    "AX-T01": _case_ax_t01, "AX-K01": _case_ax_k01, "AX-T02": _case_ax_t02,
    "AX-R01": _case_ax_r01,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
