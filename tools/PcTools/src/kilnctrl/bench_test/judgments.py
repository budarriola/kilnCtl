"""Pure judgment functions for the `smoke` suite's cases.

Deliberately separated from cases_smoke.py's "fetch data from the board"
half: every function here takes already-fetched, plain-Python data
(strings/dicts/numbers) and returns a ``CaseResult`` with no I/O of its
own. That is what makes each one directly, cheaply unit-testable with
synthetic inputs (tools/PcTools/tests/test_bench_test_judgments.py) without
mocking HTTP or a UART link -- and per feedback_negative_test_every_check,
every one of these has at least one test that feeds it a bad input and
confirms it reports FAIL/INCONCLUSIVE rather than PASS.
"""
from __future__ import annotations

import re
from typing import Any, Optional

from .registry import CaseResult, Verdict


def judge_tree_provenance(dirty_files: "list[str]", sensitive_files: "list[str]") -> CaseResult:
    """ST-05: FAIL only if any dirty file matches flash_provenance's
    SENSITIVE_PATTERNS; an ordinary dirty tree is never refused."""
    if sensitive_files:
        return CaseResult(
            Verdict.FAIL,
            reason=f"sensitive dirty files present: {', '.join(sensitive_files)}",
            observed={"dirty_files": dirty_files, "sensitive_files": sensitive_files},
        )
    return CaseResult(Verdict.PASS, observed={"dirty_files": dirty_files})


def judge_partition_table_match(report_text: str) -> CaseResult:
    """FL-01: debug_check_partition_table()'s text report."""
    if report_text.startswith("error:"):
        return CaseResult(Verdict.FAIL, reason=report_text, observed={"report": report_text})
    if "MISMATCH" in report_text:
        return CaseResult(Verdict.FAIL, reason="partition table mismatch", observed={"report": report_text})
    if "MATCH" not in report_text:
        return CaseResult(Verdict.FAIL, reason="unrecognized partition report", observed={"report": report_text})
    return CaseResult(Verdict.PASS, observed={"report": report_text})


def judge_running_partition(running_label: Optional[str]) -> CaseResult:
    """FL-02: RUNNING must be `app` (the otadata gap, CLAUDE.md flash section)."""
    if running_label == "app":
        return CaseResult(Verdict.PASS, observed={"running": running_label})
    return CaseResult(
        Verdict.FAIL,
        reason=f"running partition is {running_label!r}, expected 'app'",
        observed={"running": running_label},
        expected={"running": "app"},
    )


def judge_archived_elf_matches(find_elf_text: str) -> CaseResult:
    """FL-03/FL-09: find_crash_elf()/find_safty_crash_elf()'s text report."""
    if find_elf_text.startswith("error:"):
        return CaseResult(Verdict.FAIL, reason=find_elf_text, observed={"report": find_elf_text})
    return CaseResult(Verdict.PASS, observed={"report": find_elf_text})


def judge_boot_guard(data: dict) -> CaseResult:
    """FL-04: recovery_mode False, boot_count <= 1 (a higher count on a
    board just flashed elsewhere in the run is a FAIL -- the footgun of
    docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md)."""
    recovery_mode = data.get("recovery_mode")
    boot_count = data.get("boot_count")
    if recovery_mode is not False:
        return CaseResult(Verdict.FAIL, reason=f"recovery_mode={recovery_mode!r}, expected False", observed=data)
    if not isinstance(boot_count, int) or boot_count > 1:
        return CaseResult(Verdict.FAIL, reason=f"boot_count={boot_count!r}, expected <= 1", observed=data)
    return CaseResult(Verdict.PASS, observed=data)


def judge_recovery_image_sized(recovery_row: Optional[dict], recovery_bin_size: Optional[int]) -> CaseResult:
    """FL-05: recovery row must exist; the size half is NOT_RUN with no
    local recovery.bin build, per the plan's stated exception."""
    if recovery_row is None:
        return CaseResult(Verdict.FAIL, reason="no 'recovery' partition row reported", observed={})
    if recovery_bin_size is None:
        return CaseResult(
            Verdict.NOT_RUN,
            reason="no local firmware/KilnFW_recovery/build/recovery.bin to size against",
            observed={"recovery_row": recovery_row},
        )
    if recovery_bin_size > recovery_row.get("size", 0):
        return CaseResult(
            Verdict.FAIL,
            reason=f"recovery.bin ({recovery_bin_size} B) exceeds partition size ({recovery_row.get('size')} B)",
            observed={"recovery_row": recovery_row, "recovery_bin_size": recovery_bin_size},
        )
    return CaseResult(Verdict.PASS, observed={"recovery_row": recovery_row, "recovery_bin_size": recovery_bin_size})


def judge_coredump_readable(status: Optional[int], body: Any) -> CaseResult:
    """FL-06: route answers 200 with a well-formed body (a stored dump or none)."""
    if status != 200:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/coredump/info returned {status}", observed={"status": status})
    if not isinstance(body, dict):
        return CaseResult(Verdict.FAIL, reason="response body was not a JSON object", observed={"body": body})
    return CaseResult(Verdict.PASS, observed={"body": body})


def judge_cfgfs_state(data: dict) -> CaseResult:
    """FL-07: record-only (plan §7 owner decision 6) -- the `cfg` LittleFS
    partition is unformatted/unmounted on the bench board today
    (CLAUDE.md/CONFIG_FILESYSTEM.md), so a pending/unknown format state is
    expected, not a defect. This case never FAILs: `format_pending` False
    is PASS, anything else is INCONCLUSIVE with the full state recorded so
    a human can review it, never silently dropped."""
    pending = data.get("format_pending", data.get("pending"))
    if pending is not False:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"format_pending={pending!r}, not confirmed False (record-only, plan §7 decision 6)",
            observed=data,
        )
    return CaseResult(Verdict.PASS, observed=data)


def judge_pico_slot_metadata(commit: Optional[str], boot_reason: Optional[str]) -> CaseResult:
    """FL-08: commit is a real hash; boot reason is power_on/sw_reset, not watchdog."""
    if not commit or not re.match(r"^[0-9a-fA-F]{6,40}$", commit):
        return CaseResult(Verdict.FAIL, reason=f"commit {commit!r} is not a real hash", observed={"commit": commit})
    if boot_reason == "watchdog":
        return CaseResult(Verdict.FAIL, reason="boot reason is watchdog", observed={"boot_reason": boot_reason})
    if boot_reason not in ("power_on", "sw_reset"):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"unrecognized boot reason {boot_reason!r}",
            observed={"boot_reason": boot_reason, "commit": commit},
        )
    return CaseResult(Verdict.PASS, observed={"commit": commit, "boot_reason": boot_reason})


def judge_stack_margin(report_text: str, min_free_bytes: Optional[int] = None) -> CaseResult:
    """SK-01: every registered task alive, no unexpected 'not running'; the
    baseline-vs-committed-record comparison is left to Wave 1d
    (stack_margin_baseline.load_records()) -- wave 0 only checks for an
    outright error or an unexplained missing task."""
    if report_text.startswith("error:"):
        return CaseResult(Verdict.FAIL, reason=report_text, observed={"report": report_text})
    return CaseResult(Verdict.PASS, observed={"report": report_text})


def judge_stack_margin_against_baseline(entries, baseline_by_name: dict, min_free_bytes: Optional[int] = None) -> CaseResult:
    """SK-01/SK-02 (wave 1d): compares a live ``StackMarginEntry`` reading
    against the committed baseline's worst-case-across-conditions figure
    per task name (``stack_margin_baseline.worst_case_across_conditions()``).

    Per plan §3.3's note: a task with no baseline record at all is
    INCONCLUSIVE for that task (not FAIL -- there is nothing to compare
    against yet) rather than sinking the whole case; a task that regressed
    below its own committed worst case is FAIL. ``min_free_bytes``, when
    given (SK-02's absolute floor), FAILs any live task under that many
    bytes free regardless of what the baseline says -- the httpd stack blob
    class (project memory project_httpd_stack_blob_class) is exactly a task
    that looked fine relative to its own history but was dangerously close
    in absolute terms.

    A dead (``alive=False``) task is never scored against a byte figure --
    it FAILs outright, since a task that was never created or was deleted
    is not "using less stack than expected", it is missing."""
    if not entries:
        return CaseResult(Verdict.FAIL, reason="device reported no instrumented tasks", observed={})

    dead = [e.name for e in entries if not e.alive]
    if dead:
        return CaseResult(Verdict.FAIL, reason=f"task(s) not running: {', '.join(dead)}", observed={"dead": dead})

    below_floor = []
    if min_free_bytes is not None:
        below_floor = [
            {"task": e.name, "hwm_bytes": e.hwm_bytes}
            for e in entries
            if e.hwm_bytes < min_free_bytes
        ]

    regressed = []
    inconclusive_tasks = []
    for e in entries:
        base = baseline_by_name.get(e.name)
        if base is None:
            inconclusive_tasks.append(e.name)
            continue
        if not base.alive:
            continue
        if e.hwm_bytes < base.hwm_bytes:
            regressed.append({"task": e.name, "hwm_bytes": e.hwm_bytes, "baseline_hwm_bytes": base.hwm_bytes})

    observed = {
        "entries": [{"name": e.name, "hwm_bytes": e.hwm_bytes, "configured_stack_bytes": e.configured_stack_bytes} for e in entries],
        "below_floor": below_floor,
        "regressed": regressed,
        "no_baseline": inconclusive_tasks,
    }

    if below_floor:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(below_floor)} task(s) below the {min_free_bytes} B absolute floor",
            observed=observed,
        )
    if regressed:
        names = ", ".join(r["task"] for r in regressed)
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(regressed)} task(s) below their committed baseline: {names}",
            observed=observed,
        )
    if inconclusive_tasks:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"no committed baseline for: {', '.join(inconclusive_tasks)}",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_pico_archive_with_description(find_elf_text: str, description_exists) -> CaseResult:
    """FL-09 (wave 1d override): ``find_safty_crash_elf()``'s text report,
    downgraded from PASS to INCONCLUSIVE when the found ELF has no sibling
    ``project_description.json`` -- SaftyFW's CMake/pico-sdk build never
    produces one, and ``elf_archive/`` deliberately never bundles one even
    for KilnFW (2026-09-19 revert; that file's absolute-path gdbinit
    reference goes stale). ``read_esp_coredump`` needs that file to actually
    symbolize, so its absence is an expected, permanent gap for this
    processor's archive -- record-only, never FAIL, per the plan's "FL-09
    record-only INCONCLUSIVE never FAIL" rule. A genuine no-match (no ELF
    archived at all) keeps the ordinary FAIL behavior.

    ``description_exists`` is injected (a ``str -> bool`` callable) so this
    stays a pure function for unit testing -- no filesystem access here."""
    if find_elf_text.startswith("error:"):
        return CaseResult(Verdict.FAIL, reason=find_elf_text, observed={"report": find_elf_text})
    m = _FL09_FOUND_RE.search(find_elf_text)
    if not m:
        return CaseResult(Verdict.PASS, observed={"report": find_elf_text})
    elf_path = m.group(1)
    if description_exists(elf_path):
        return CaseResult(Verdict.PASS, observed={"report": find_elf_text, "elf_path": elf_path})
    return CaseResult(
        Verdict.INCONCLUSIVE,
        reason=(
            f"archived ELF {elf_path} has no sibling project_description.json -- "
            "read_esp_coredump cannot symbolize without it; elf_archive/ deliberately "
            "never bundles this file (record-only, plan FL-09 rule)"
        ),
        observed={"report": find_elf_text, "elf_path": elf_path},
    )


_FL09_FOUND_RE = re.compile(r"found (\S+\.elf)", re.IGNORECASE)


def judge_pico_stack_margins(tasks: "list[dict]", min_fraction: float = 0.25) -> CaseResult:
    """SK-03: every task's free >= 25% of configured (memory
    project_saftyfw_minimal_stack_overflows -- the flat interim rule per
    plan §7 owner decision 4, pending a wave 1 Pico baseline)."""
    if not tasks:
        return CaseResult(Verdict.FAIL, reason="no task entries reported", observed={"tasks": tasks})
    failing = []
    for t in tasks:
        # GET /api/saftyfw_stack_margin reports `stack_total_words` and
        # `high_water_words` (FreeRTOS uxTaskGetStackHighWaterMark(): the
        # minimum FREE stack ever observed, i.e. the margin floor). The
        # `configured`/`free` spelling is kept for callers that pre-shape it.
        configured = t.get("configured") or t.get("size") or t.get("stack_total_words")
        free = t.get("free")
        if free is None:
            free = t.get("high_water_words")
        if t.get("measured") is False:
            failing.append({"task": t.get("name"), "reason": "not measured"})
            continue
        if not configured or free is None:
            failing.append({"task": t.get("name"), "reason": "missing configured/free"})
            continue
        if free < min_fraction * configured:
            failing.append({"task": t.get("name"), "free": free, "configured": configured})
    if failing:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(failing)} task(s) below {min_fraction:.0%} free",
            observed={"failing": failing, "tasks": tasks},
        )
    return CaseResult(Verdict.PASS, observed={"tasks": tasks})


def judge_heap_dram_floor(dram_largest_free_before: Optional[int], dram_largest_free_after: Optional[int],
                           unacknowledged_crash: bool, floor_bytes: int = 11900) -> CaseResult:
    """SK-04: internal DRAM largest free block never below 11.9 kB; no
    UNACKNOWLEDGED CRASH REPORT banner."""
    if unacknowledged_crash:
        return CaseResult(Verdict.FAIL, reason="unacknowledged crash report present", observed={})
    for label, value in (("before", dram_largest_free_before), ("after", dram_largest_free_after)):
        if value is None:
            continue
        if value < floor_bytes:
            return CaseResult(
                Verdict.FAIL,
                reason=f"DRAM largest free block {label}={value} B < floor {floor_bytes} B",
                observed={"before": dram_largest_free_before, "after": dram_largest_free_after},
            )
    return CaseResult(Verdict.PASS, observed={"before": dram_largest_free_before, "after": dram_largest_free_after})


def judge_commissioning_readback(commissioning: dict, esp_max_temp_c: Optional[float]) -> CaseResult:
    """SP-01: commissioned true, stale false; abs_max_temp_c equal on both
    sides and > 0 (constraint 19 of §6)."""
    if not commissioning.get("commissioned", False):
        return CaseResult(Verdict.FAIL, reason="commissioned=False", observed=commissioning)
    if commissioning.get("stale", True):
        return CaseResult(Verdict.FAIL, reason="stale=True", observed=commissioning)
    pico_max = commissioning.get("abs_max_temp_c")
    if pico_max is None:
        # GET /api/safety/commissioning carries the record as
        # params:[{name,set,value?}] -- `value` is omitted for an unset param
        # (safety_cfg_http_client.get_commissioning), so an unset ceiling
        # stays None here and fails below rather than reading as 0.
        for param in commissioning.get("params") or []:
            if isinstance(param, dict) and param.get("name") == "abs_max_temp_c":
                pico_max = param.get("value") if param.get("set", True) else None
                break
    if not pico_max or pico_max <= 0:
        return CaseResult(Verdict.FAIL, reason=f"abs_max_temp_c={pico_max!r}, expected > 0", observed=commissioning)
    if esp_max_temp_c is not None and abs(esp_max_temp_c - pico_max) > 1e-6:
        return CaseResult(
            Verdict.FAIL,
            reason=f"abs_max_temp_c differs: pico={pico_max} esp={esp_max_temp_c}",
            observed={"commissioning": commissioning, "esp_max_temp_c": esp_max_temp_c},
        )
    return CaseResult(Verdict.PASS, observed={"commissioning": commissioning, "esp_max_temp_c": esp_max_temp_c})


def judge_status_diag_consistency(link_up: bool, state: Optional[str], boot_reason: Optional[str],
                                   trip_reason: Optional[int], trip_mask: Optional[int] = None) -> CaseResult:
    """SP-02: link up, state armed/idle as appropriate, boot reason not
    watchdog, trip_reason 0, and (when both fields were actually parsed)
    trip_mask consistent with trip_reason via the same formula
    (`link_frame_trip_mask_for_reason()`).

    `trip_reason=None` means the report did not carry the field at all --
    this must NOT be treated as "0 / no trip" (that silently passes a
    board that IS tripped but whose report couldn't be parsed) -- it is
    INCONCLUSIVE instead, distinct from both PASS and the FAIL a real
    nonzero trip_reason produces."""
    if not link_up:
        return CaseResult(Verdict.FAIL, reason="link is not up", observed={"link_up": link_up})
    if boot_reason == "watchdog":
        return CaseResult(Verdict.FAIL, reason="boot reason is watchdog", observed={"boot_reason": boot_reason})
    if trip_reason is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="trip_reason not reported (diag text did not parse) -- cannot judge trip state",
            observed={"trip_reason": trip_reason},
        )
    if trip_reason != 0:
        return CaseResult(Verdict.FAIL, reason=f"trip_reason={trip_reason}, expected 0", observed={"trip_reason": trip_reason})
    expected_mask = safety_trip_mask_for_reason(trip_reason)
    if trip_mask is not None and trip_mask != expected_mask:
        return CaseResult(
            Verdict.FAIL,
            reason=(
                f"trip_reason={trip_reason} implies trip_mask={expected_mask:#06x}, "
                f"but diag reports trip_mask={trip_mask:#06x} -- status/diag are inconsistent"
            ),
            observed={"trip_reason": trip_reason, "trip_mask": trip_mask, "expected_mask": expected_mask},
        )
    if state not in ("armed", "idle"):
        return CaseResult(Verdict.FAIL, reason=f"state={state!r}, expected armed or idle", observed={"state": state})
    return CaseResult(Verdict.PASS, observed={"link_up": link_up, "state": state, "boot_reason": boot_reason, "trip_reason": trip_reason, "trip_mask": trip_mask})


def judge_estop_verify(status_text: str) -> CaseResult:
    """SP-05: jumper fitted / not asserted (memory
    project_estop_jumper_is_fitted) -- read-only, off the cached GET_STATUS
    flags text (srv.safety_get_status(), same source SP-02 parses). This
    case must NEVER call POST /api/estop/verify: that route is an admin
    write recording an operator's physical verification of the interlock
    (diagnostics_http.c, estop_verification.h), not a state read, so it has
    no business inside a read-only smoke suite.

    ``status_text`` is SafetyStatus.describe()'s rendering: flag labels
    joined with '; ', or "error: ..." when the query itself failed
    (SafetyQueryError, e.g. serial_link.py's "no serial port open - connect
    first" or link_hub.py's "hub did not respond ..."). SAFETY_FLAG_LABELS
    maps SafetyFlag.ESTOP to the literal label "E-stop asserted"
    (devices_safety.py), so that substring is what appears when the bit is
    set. A down serial hub/link is INCONCLUSIVE, not FAIL -- it says nothing
    about the board's actual E-stop state, and is a known current condition
    (another process holding the hub), not a board defect. This also covers
    the case where the status query itself succeeds but the safety link to
    the Pico is down: describe() then renders "no flags set" (every flag,
    including SAFETY_FLAG_LABELS' "E-stop asserted", is simply absent, not
    known-clear) and "never received" for the context age -- with no data
    at all, treating that absence as a PASS would be a false PASS. Require
    SAFETY_FLAG_LABELS[SafetyFlag.LINK_UP] ("link up") present and reject
    "never received", the same link_up check SP-02 makes off this same
    text (cases_smoke.py's _case_sp02)."""
    if status_text is None:
        return CaseResult(Verdict.FAIL, reason="no status text reported", observed={"status_text": status_text})
    lowered = status_text.lower()
    if lowered.startswith("error:"):
        if "no serial port" in lowered or "hub did not respond" in lowered:
            return CaseResult(Verdict.INCONCLUSIVE, reason=status_text, observed={"status_text": status_text})
        return CaseResult(Verdict.FAIL, reason=status_text, observed={"status_text": status_text})
    if "link up" not in lowered or "never received" in lowered:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="safety link is not up -- E-stop state cannot be judged from absent flags",
            observed={"status_text": status_text},
        )
    if "e-stop asserted" in lowered:
        return CaseResult(
            Verdict.FAIL,
            reason="E-stop asserted (per safety_get_status())",
            observed={"status_text": status_text},
        )
    return CaseResult(Verdict.PASS, observed={"status_text": status_text})


def judge_web_render(html: Optional[str], landmark_id: Optional[str], expect_nav: bool,
                      error: Optional[str] = None) -> CaseResult:
    """WEB-*-01: page answers 200 (the fetch layer raises on non-2xx before
    this is reached, so `error` carries that text), its known landmark
    element id is present, and (except `/login`, which loads before the
    shared chrome) `nav.js` is referenced."""
    if error is not None:
        return CaseResult(Verdict.FAIL, reason=error, observed={})
    if not html:
        return CaseResult(Verdict.FAIL, reason="empty response body", observed={})
    if landmark_id and f'id="{landmark_id}"' not in html:
        return CaseResult(
            Verdict.FAIL,
            reason=f"landmark id={landmark_id!r} not found in page",
            observed={"landmark": landmark_id, "body_len": len(html)},
        )
    if expect_nav and "nav.js" not in html:
        return CaseResult(Verdict.FAIL, reason="nav.js script reference missing", observed={"body_len": len(html)})
    return CaseResult(Verdict.PASS, observed={"landmark": landmark_id, "nav_js": expect_nav, "body_len": len(html)})


def judge_nav_menu(nav_js_text: Optional[str], href_count: Optional[int], expected_count: int = 16,
                    has_group_expand: Optional[bool] = None) -> CaseResult:
    """WEB-X-01: the shared nav.js menu carries exactly the links the plan
    names, and supports an expanding sub-group (the Safety group,
    `children`/`activeFor` per nav.js's 2026-08-27 rework) for the
    "auto-expands the current group" half of the case.

    ``expected_count`` bumped 15 -> 16 2026-09-21: `e3de6122`
    (LIVE_PROFILE_EDIT_PLAN.md) added `/live_profile` ("Edit running
    firing") to nav.js's menu after this case's 15 was set, which turned it
    red against a healthy, correctly-updated board -- same "count went
    stale the moment new content landed" class WEB-X-03's own docstring
    already names for the route-tier table (that one is now derived at run
    time rather than hardcoded; this simpler case still hardcodes the
    count, so it needs the same bump by hand whenever the menu grows)."""
    if nav_js_text is None:
        return CaseResult(Verdict.FAIL, reason="GET /nav.js failed", observed={})
    if href_count != expected_count:
        return CaseResult(
            Verdict.FAIL,
            reason=f"nav.js has {href_count} href entries, expected {expected_count}",
            observed={"href_count": href_count},
        )
    if has_group_expand is False:
        return CaseResult(
            Verdict.FAIL,
            reason="nav.js has no expanding-group support (activeFor/children)",
            observed={"href_count": href_count},
        )
    return CaseResult(Verdict.PASS, observed={"href_count": href_count, "has_group_expand": has_group_expand})


def judge_route_tier_sweep(results: "list[dict]") -> CaseResult:
    """WEB-X-03: every GET route actually exercised (OPEN/SAFETY_REDUCE
    answering without a session, ADMIN answering 401/redirect-to-login
    without one when the board currently has web auth enabled, and
    answering normally when it does not -- see cases_web.py's docstring
    for why an ADMIN route's 200 is not itself a failure on a bench with
    auth off) must have behaved as classified. Non-GET rows are recorded
    but never invoked in this read-only wave (see module docstring) and
    never fail this case on their own."""
    violations = [r for r in results if r.get("exercised") and not r.get("ok")]
    if violations:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(violations)} route(s) did not match their declared tier",
            observed={"violations": violations, "total_rows": len(results), "rows": results},
        )
    exercised = sum(1 for r in results if r.get("exercised"))
    if exercised == 0:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no GET routes were exercised",
            observed={"total_rows": len(results), "rows": results},
        )
    return CaseResult(
        Verdict.PASS,
        observed={"total_rows": len(results), "exercised": exercised, "rows": results},
    )


def judge_rate_guard_consistency(safety_side: dict, esp_side: dict) -> CaseResult:
    """SP-07: consistent read-back between the two sides; not written."""
    if safety_side.get("max_rate_c_per_min") is None or esp_side.get("max_rate_c_per_min") is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="rate guard fields missing on one side",
            observed={"safety_side": safety_side, "esp_side": esp_side},
        )
    if safety_side.get("max_rate_c_per_min") != esp_side.get("max_rate_c_per_min"):
        return CaseResult(
            Verdict.FAIL,
            reason="rate guard values differ between processors",
            observed={"safety_side": safety_side, "esp_side": esp_side},
        )
    return CaseResult(Verdict.PASS, observed={"safety_side": safety_side, "esp_side": esp_side})


# ---------------------------------------------------------------------------
# LCD suite judgments (plan §8 Wave 1c). "targets" throughout is the dict
# list_tap_targets() returns: [{"name","cx","cy","hidden"}, ...].
# ---------------------------------------------------------------------------

def _find_target(targets: "list[dict]", name: str) -> "Optional[dict]":
    for t in targets:
        if t.get("name") == name:
            return t
    return None


def judge_lcd_home_idle(page: str, targets: "list[dict]",
                         start_matches_accent4: Optional[bool],
                         pause_is_hidden: Optional[bool]) -> CaseResult:
    """LCD-01: home page, idle. Start button present and tappable, Pause
    hidden, and (when a capture was available) Start reads the ACCENT_4
    color. A missing camera capture degrades the color half to
    INCONCLUSIVE rather than failing the whole case on a busy webcam
    (CLAUDE.md: ffmpeg exit -5 means busy, never treated as a board defect).
    """
    observed = {"page": page, "targets": targets}
    if page != "home":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'home'", observed=observed)
    start = _find_target(targets, "start")
    if start is None or start.get("hidden"):
        return CaseResult(Verdict.FAIL, reason="Start target missing or hidden on home/idle", observed=observed)
    pause = _find_target(targets, "pause")
    if pause is not None and not pause.get("hidden") and pause_is_hidden is not False:
        # pause_is_hidden (from a camera sample) can override a firmware
        # 'hidden' flag disagreement; absent camera data, trust the flag.
        return CaseResult(Verdict.FAIL, reason="Pause target is not hidden on home/idle", observed=observed)
    if start_matches_accent4 is False:
        return CaseResult(Verdict.FAIL, reason="Start button region does not read as ACCENT_4", observed=observed)
    # Only the checks a capture could actually have answered make this
    # INCONCLUSIVE. A board that does not list a hidden Pause target at all
    # legitimately yields pause_is_hidden=None with a perfectly good frame --
    # requiring it unconditionally made LCD-01 permanently INCONCLUSIVE on
    # such a board, which reads like a broken camera rather than a pass.
    if start_matches_accent4 is None or (pause is not None and pause_is_hidden is None):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no camera capture available (webcam busy or unreachable) -- widget state checked, color not",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_config_hub(page: str, targets: "list[dict]",
                          expected_tiles: "tuple[str, ...]" = (
                              "Profiles", "Temperature", "Network", "Diagnostics",
                          )) -> CaseResult:
    """LCD-08: config hub reached by tapping Menu. `expected_tiles` omits
    Touch Calibration by default since the plan marks it 'if present' --
    callers that know the board has it should pass the 5-tuple."""
    observed = {"page": page, "targets": targets}
    if page != "config":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'config'", observed=observed)
    names = {t.get("name") for t in targets if not t.get("hidden")}
    missing = [t for t in expected_tiles if t not in names]
    if missing:
        return CaseResult(Verdict.FAIL, reason=f"missing config tile(s): {', '.join(missing)}", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_no_scroll_budget(pages_targets: "dict[str, dict]") -> CaseResult:
    """LCD-21: no tap target on any visited page reports cy > 320 with
    truncated False (memory feedback_lcd_no_scrolling -- a truncated list
    is a reporting limit, not evidence of an actual off-screen target, so
    it is excluded from the offending set rather than counted as a pass).
    `pages_targets` is {page_name: list_tap_targets() dict}."""
    if not pages_targets:
        return CaseResult(Verdict.NOT_RUN, reason="no pages were visited to check", observed={})
    offenders: "dict[str, list]" = {}
    for page, result in pages_targets.items():
        targets = result.get("targets", [])
        truncated = result.get("truncated", False)
        if truncated:
            continue
        bad = [t for t in targets if t.get("cy", 0) > 320]
        if bad:
            offenders[page] = bad
    observed = {"pages": list(pages_targets.keys()), "offenders": offenders}
    if offenders:
        return CaseResult(
            Verdict.FAIL,
            reason=f"target(s) below the 320px no-scroll budget on: {', '.join(offenders)}",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


# ---------------------------------------------------------------------------
# LCD-02/03/04/09/14/16/19 (Wave 2, plan doc §3.8/§8). Same split as the
# Wave 1c block above: these take already-fetched plain data and return a
# CaseResult, with no I/O of their own -- cases_lcd.py does the fetching
# (UI_TEST navigation, thermo/profile/safety reads, best-effort webcam
# sampling via lcd_sampler.py).
# ---------------------------------------------------------------------------

_DIAG_TITLES = (
    "Safety & Board Health", "Thermocouple Faults", "Trip Detail",
    "Relay Life", "Crash Report",
)


def judge_lcd_home_firing(page: str, targets: "list[dict]",
                           start_reads_stop: bool,
                           pause_matches_accent1: Optional[bool],
                           progress_samples: "list[float]",
                           profile_name_greyed: Optional[bool],
                           profile_name_tap_noop: Optional[bool]) -> CaseResult:
    """LCD-02: home page while HP-01 is running. `progress_samples` is a
    list of `ProfileExecStatus.segment_elapsed_s` readings taken ~60s apart
    -- a real, already-wire-carried monotonic proxy for "the profile is
    progressing", used instead of trying to derive a percentage from a
    single webcam pixel sample. Camera-only sub-checks (`pause_matches_accent1`)
    degrade to INCONCLUSIVE, never FAIL, on a missing capture -- same
    discipline as judge_lcd_home_idle."""
    observed = {
        "page": page, "start_reads_stop": start_reads_stop,
        "pause_matches_accent1": pause_matches_accent1,
        "progress_samples": progress_samples,
        "profile_name_greyed": profile_name_greyed,
        "profile_name_tap_noop": profile_name_tap_noop,
    }
    if page != "home":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'home'", observed=observed)
    if not start_reads_stop:
        return CaseResult(Verdict.FAIL, reason="Start widget does not read 'Stop' while firing", observed=observed)
    if pause_matches_accent1 is False:
        return CaseResult(Verdict.FAIL, reason="Pause region does not read as ACCENT_1 while firing", observed=observed)
    if len(progress_samples) >= 2 and progress_samples[-1] <= progress_samples[0]:
        return CaseResult(
            Verdict.FAIL,
            reason=f"progress did not advance between samples ({progress_samples[0]} -> {progress_samples[-1]})",
            observed=observed,
        )
    if profile_name_greyed is False:
        return CaseResult(Verdict.FAIL, reason="profile-name button is not greyed while firing", observed=observed)
    if profile_name_tap_noop is False:
        return CaseResult(Verdict.FAIL, reason="tapping the greyed profile-name button changed the page", observed=observed)
    inconclusive_bits = [
        pause_matches_accent1 is None,
        len(progress_samples) < 2,
        profile_name_greyed is None,
        profile_name_tap_noop is None,
    ]
    if any(inconclusive_bits):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="one or more firing-window checks could not be exercised (no camera capture or no profile-name widget)",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_home_paused(pause_label: Optional[str], duties: "list[float]") -> CaseResult:
    """LCD-03: HP-04's paused window. Pause widget must read 'Resume' and
    every zone's duty must read 0 while paused."""
    observed = {"pause_label": pause_label, "duties": duties}
    if pause_label is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no Pause widget label was captured during the paused window",
            observed=observed,
        )
    if str(pause_label).lower() != "resume":
        return CaseResult(
            Verdict.FAIL,
            reason=f"Pause widget reads {pause_label!r} while paused, expected 'Resume'",
            observed=observed,
        )
    if not duties:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no duty samples were captured during the paused window", observed=observed)
    nonzero = [d for d in duties if d]
    if nonzero:
        return CaseResult(Verdict.FAIL, reason=f"duty(ies) nonzero while paused: {nonzero}", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_home_tripped(strip_visible_before: bool,
                            strip_matches_accent5_before: Optional[bool],
                            strip_off_after_clear: Optional[bool]) -> CaseResult:
    """LCD-04: observes OT-B01's S6a trip window. The strip must be visible
    and read ACCENT_5 while the trip is latched, then be gone/off after
    `safety_clear_trip()`. Never originates a trip itself -- if none is
    latched when this runs, the case function reports NOT_RUN before this
    is even called."""
    observed = {
        "strip_visible_before": strip_visible_before,
        "strip_matches_accent5_before": strip_matches_accent5_before,
        "strip_off_after_clear": strip_off_after_clear,
    }
    if not strip_visible_before:
        return CaseResult(Verdict.FAIL, reason="trip strip was not visible while a trip was latched", observed=observed)
    if strip_matches_accent5_before is False:
        return CaseResult(Verdict.FAIL, reason="trip strip region does not read as ACCENT_5 while tripped", observed=observed)
    if strip_off_after_clear is False:
        return CaseResult(Verdict.FAIL, reason="trip strip is still visible/lit after safety_clear_trip()", observed=observed)
    if strip_matches_accent5_before is None or strip_off_after_clear is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no camera capture available to confirm the trip strip's color",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_profiles_picker(page: str, rows: "list[dict]", paging_present: bool,
                               new_icon_present: bool, detail_page: Optional[str],
                               max_rows: int = 4) -> CaseResult:
    """LCD-09: profiles picker reached by tapping Profiles from the hub.
    `rows` is the subset of tap targets named `profile_row_*`."""
    observed = {
        "page": page, "row_count": len(rows), "paging_present": paging_present,
        "new_icon_present": new_icon_present, "detail_page": detail_page,
    }
    if page != "profiles":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'profiles'", observed=observed)
    if len(rows) > max_rows:
        return CaseResult(Verdict.FAIL, reason=f"{len(rows)} rows shown, expected <= {max_rows}", observed=observed)
    favorites = [r for r in rows if r.get("starred")]
    if favorites and any(not r.get("starred") for r in rows[: len(favorites)]):
        return CaseResult(Verdict.FAIL, reason="favorite row(s) are not sorted first", observed=observed)
    if not paging_present:
        return CaseResult(Verdict.FAIL, reason="no paging indicator found in the topbar", observed=observed)
    if not new_icon_present:
        return CaseResult(Verdict.FAIL, reason="New profile icon missing from the topbar", observed=observed)
    if rows and detail_page != "profile_detail":
        return CaseResult(
            Verdict.FAIL,
            reason=f"tapping a row opened {detail_page!r}, expected 'profile_detail'",
            observed=observed,
        )
    if not rows:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no profile rows present to confirm row-tap navigation", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_temperature_page(page: str, zone_rows: "dict[int, float]",
                                readings: "dict[int, float]",
                                safety_on: Optional[bool], expect_safety_on: bool,
                                tolerance_c: float = 1.0) -> CaseResult:
    """LCD-14: three zone rows within `tolerance_c` of `thermo_read()`, and
    (post-rework) the Safety (K4) line matching whether a firing is active.
    A pre-rework board with no Safety line target degrades that half to
    INCONCLUSIVE rather than FAIL (plan: "[pre: line absent]")."""
    observed = {
        "page": page, "zone_rows": zone_rows, "readings": readings,
        "safety_on": safety_on, "expect_safety_on": expect_safety_on,
    }
    if page != "temperature":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'temperature'", observed=observed)
    if not zone_rows:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no zone rows with a numeric value were reported", observed=observed)
    mismatches = {}
    for zone, shown in zone_rows.items():
        actual = readings.get(zone)
        if shown is None or actual is None:
            continue
        if abs(shown - actual) > tolerance_c:
            mismatches[zone] = (shown, actual)
    if mismatches:
        return CaseResult(
            Verdict.FAIL,
            reason=f"zone value(s) differ from thermo_read() by more than {tolerance_c}C: {mismatches}",
            observed=observed,
        )
    if safety_on is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no Safety (K4) line state reported (pre-rework LCD)", observed=observed)
    if bool(safety_on) != bool(expect_safety_on):
        return CaseResult(
            Verdict.FAIL,
            reason=f"Safety (K4) line reads on={safety_on}, expected {expect_safety_on}",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_diagnostics_pages(titles_seen: "list[str]", relay_life_has_reset: Optional[bool],
                                 crash_report_visible_entries: Optional[int],
                                 heap_diff_pct: Optional[float],
                                 expected_titles: "tuple[str, ...]" = _DIAG_TITLES,
                                 max_heap_diff_pct: float = 10.0) -> CaseResult:
    """LCD-16: the 5 diagnostics sub-pages reached in order, Relay Life has
    no Reset button (post-rework), Crash Report shows none, and the LCD's
    own reported heap value is within `max_heap_diff_pct` of
    `get_heap_status()`."""
    observed = {
        "titles_seen": titles_seen, "relay_life_has_reset": relay_life_has_reset,
        "crash_report_visible_entries": crash_report_visible_entries,
        "heap_diff_pct": heap_diff_pct,
    }
    if list(titles_seen) != list(expected_titles):
        return CaseResult(
            Verdict.FAIL,
            reason=f"diagnostics titles seen {titles_seen!r}, expected {list(expected_titles)!r}",
            observed=observed,
        )
    if relay_life_has_reset:
        return CaseResult(Verdict.FAIL, reason="Relay Life page still shows a Reset button (pre-rework only)", observed=observed)
    if crash_report_visible_entries:
        return CaseResult(
            Verdict.FAIL,
            reason=f"Crash Report page shows {crash_report_visible_entries} visible entrie(s), expected none",
            observed=observed,
        )
    if heap_diff_pct is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="could not compare the board-health page's heap value to get_heap_status()",
            observed=observed,
        )
    if heap_diff_pct > max_heap_diff_pct:
        return CaseResult(
            Verdict.FAIL,
            reason=f"board-health heap value differs from get_heap_status() by {heap_diff_pct:.1f}%, expected <= {max_heap_diff_pct:.0f}%",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_pin_lock(keypad_raised: Optional[bool], wrong_pin_refused: Optional[bool],
                        right_pin_started: Optional[bool], stop_not_gated: Optional[bool]) -> CaseResult:
    """LCD-19: after `lcd_timeout_min`, Start raises the keypad; a wrong PIN
    is refused; the right PIN starts; Stop is NEVER gated (plan: a tap on
    Stop during a firing with the lock engaged must stop it with no PIN
    prompt) -- this last check is the one that matters for safety, so it is
    checked even though the others may be INCONCLUSIVE."""
    observed = {
        "keypad_raised": keypad_raised, "wrong_pin_refused": wrong_pin_refused,
        "right_pin_started": right_pin_started, "stop_not_gated": stop_not_gated,
    }
    if keypad_raised is False:
        return CaseResult(Verdict.FAIL, reason="Start tap after the LCD timeout did not raise the PIN keypad", observed=observed)
    if wrong_pin_refused is False:
        return CaseResult(Verdict.FAIL, reason="a wrong PIN was not refused", observed=observed)
    if right_pin_started is False:
        return CaseResult(Verdict.FAIL, reason="the correct PIN did not start the firing", observed=observed)
    if stop_not_gated is False:
        return CaseResult(
            Verdict.FAIL,
            reason="Stop was gated behind the PIN lock; the plan requires Stop is never gated",
            observed=observed,
        )
    if None in (keypad_raised, wrong_pin_refused, right_pin_started, stop_not_gated):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="one or more PIN-lock checks could not be exercised (missing UI_TEST API or camera)",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


# ---------------------------------------------------------------------------
# HP-* / SP-* (Wave 1b: HP + SP observers, plan doc section 3.6/3.9/8)
# ---------------------------------------------------------------------------

def judge_rested(temps: "dict[int, float]", ambient_ref: float, band: float = 2.0) -> bool:
    """Rest gate (plan section 2.4/5.2 rule 7): every zone within `band` of
    `ambient_ref` AND of each other -- not merely near its own cold
    junction (memory project_autotune_needs_rested_baseline: residual heat
    biases things low if this is skipped). Plain bool, not a CaseResult --
    this is a precondition callers loop on, not a case verdict of its own.
    """
    if not temps:
        return False
    values = list(temps.values())
    if max(values) - min(values) > band:
        return False
    return all(abs(v - ambient_ref) <= band for v in values)


def judge_zone_rise_ordering(rises: "dict[int, float]", primary_zone: int, min_rise: float = 5.0) -> CaseResult:
    """HP-01: zone `primary_zone` rises >= `min_rise`; every other zone
    present rises LESS than the primary zone's own rise. Coupling on this
    fixture is real (5-12 C/duty) so other zones WILL move -- judged by
    ordering against the primary zone's rise, never by requiring them to
    stay flat (plan section 3.6)."""
    primary_rise = rises.get(primary_zone)
    if primary_rise is None:
        return CaseResult(
            Verdict.FAIL, reason=f"no rise recorded for zone {primary_zone}", observed={"rises": rises}
        )
    if primary_rise < min_rise:
        return CaseResult(
            Verdict.FAIL,
            reason=f"zone {primary_zone} rose {primary_rise:.1f}C, expected >= {min_rise:.1f}C",
            observed={"rises": rises}, expected={"min_rise_c": min_rise},
        )
    offenders = {z: r for z, r in rises.items() if z != primary_zone and r >= primary_rise}
    if offenders:
        return CaseResult(
            Verdict.FAIL,
            reason=f"zone(s) {sorted(offenders)} rose as much as or more than primary zone {primary_zone}",
            observed={"rises": rises, "primary_rise_c": primary_rise},
        )
    return CaseResult(Verdict.PASS, observed={"rises": rises, "primary_rise_c": primary_rise})


def judge_all_zones_rise(rises: "dict[int, float]", zone_mask: int, min_rise: float = 5.0) -> CaseResult:
    """HP-02: every zone named by `zone_mask` rises >= `min_rise`."""
    expected_zones = [z for z in range(3) if zone_mask & (1 << z)]
    shortfalls = {
        z: rises.get(z) for z in expected_zones if rises.get(z) is None or rises.get(z) < min_rise
    }
    if shortfalls:
        return CaseResult(
            Verdict.FAIL,
            reason=f"zone(s) {sorted(shortfalls)} did not rise >= {min_rise:.1f}C",
            observed={"rises": rises, "shortfalls": shortfalls}, expected={"min_rise_c": min_rise},
        )
    return CaseResult(Verdict.PASS, observed={"rises": rises})


def judge_relay_energized(samples: "list[tuple[str, Optional[bool]]]") -> CaseResult:
    """HP-01 / SP-06: K4 (`safety_relay_energized`) true only while the
    profile state is `running`, false the rest of the time -- each sample
    is (state_name, energized_or_None). `None` means the board did not
    report the field that tick (no host, or a stale build); if every sample
    is `None` this is INCONCLUSIVE, never a silent PASS."""
    if not samples:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no safety_relay_energized samples collected", observed={})
    reported = [(s, e) for s, e in samples if e is not None]
    if not reported:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="safety_relay_energized was never reported (no host, or a stale build)",
            observed={"samples": samples},
        )
    offenders = [(s, e) for s, e in reported if (s == "running") != bool(e)]
    if offenders:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(offenders)} sample(s) disagree: relay-energized must track RUNNING exactly",
            observed={"offenders": offenders, "samples": samples},
        )
    return CaseResult(Verdict.PASS, observed={"samples": samples})


def judge_pause_resume(paused_state: str, duties_while_paused: "list[float]", final_state: str) -> CaseResult:
    """HP-04: pause holds state and zeroes every duty; resume completes the run."""
    if paused_state != "paused":
        return CaseResult(
            Verdict.FAIL,
            reason=f"state after profiles_pause() was {paused_state!r}, expected 'paused'",
            observed={"paused_state": paused_state},
        )
    nonzero = [d for d in duties_while_paused if d]
    if nonzero:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(nonzero)} zone(s) had nonzero duty while paused",
            observed={"duties_while_paused": duties_while_paused},
        )
    if final_state != "done":
        return CaseResult(
            Verdict.FAIL,
            reason=f"profile did not reach DONE after resume (state={final_state!r})",
            observed={"final_state": final_state},
        )
    return CaseResult(Verdict.PASS, observed={"paused_state": paused_state, "final_state": final_state})


def judge_stop(state_after_stop: str, duties: "list[float]", relays: "list[bool]", acked: bool) -> CaseResult:
    """HP-05: stop leaves RUNNING, zeroes duties and relays, and the last-run
    card can be acknowledged."""
    if state_after_stop == "running":
        return CaseResult(
            Verdict.FAIL, reason="state still RUNNING after profiles_stop()", observed={"state": state_after_stop}
        )
    if any(duties):
        return CaseResult(Verdict.FAIL, reason="a zone duty is nonzero after stop", observed={"duties": duties})
    if any(relays):
        return CaseResult(
            Verdict.FAIL, reason="a relay is still commanded on after stop", observed={"relays": relays}
        )
    if not acked:
        return CaseResult(
            Verdict.FAIL, reason="profiles_ack_last_run() did not clear the last-run card", observed={"acked": acked}
        )
    return CaseResult(Verdict.PASS, observed={"state": state_after_stop, "duties": duties, "relays": relays})


def judge_unauthenticated_stop(http_status: Optional[int], state_after: str) -> CaseResult:
    """HP-06: stop is never gated -- an unauthenticated POST still succeeds
    (200, SAFETY_REDUCE tier) and the firing actually stops."""
    if http_status != 200:
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/profile_exec/stop (no session) returned {http_status!r}, expected 200",
            observed={"status": http_status},
        )
    if state_after == "running":
        return CaseResult(
            Verdict.FAIL, reason="firing still RUNNING after the unauthenticated stop", observed={"state": state_after}
        )
    return CaseResult(Verdict.PASS, observed={"status": http_status, "state": state_after})


def judge_firing_history(entries: "list[dict]", expected_name_prefix: str = "BENCH_") -> CaseResult:
    """HP-08: a completed bench run appears with the right profile name, a
    start time and an outcome."""
    if not entries:
        return CaseResult(Verdict.FAIL, reason="firing history is empty", observed={"entries": entries})
    matches = [e for e in entries if str(e.get("profile_name", e.get("name", ""))).startswith(expected_name_prefix)]
    if not matches:
        return CaseResult(
            Verdict.FAIL,
            reason=f"no history entry names a {expected_name_prefix!r} profile",
            observed={"entries": entries},
        )
    for entry in matches:
        if "start_time" not in entry and "started" not in entry:
            return CaseResult(Verdict.FAIL, reason="a matching history entry has no start-time field", observed={"entry": entry})
        if "outcome" not in entry and "state" not in entry:
            return CaseResult(Verdict.FAIL, reason="a matching history entry has no outcome field", observed={"entry": entry})
    return CaseResult(Verdict.PASS, observed={"matches": matches})


def judge_link_stats_delta(before: "dict[str, Any]", after: "dict[str, Any]") -> CaseResult:
    """SP-03: crc_errors/timeouts/broadcast_dropped deltas are 0 across a
    firing.

    `timeouts` IS compared here, deliberately. It is no longer the old
    per-exchange "GET_STATUS got no reply" counter that read a bimodal
    ~0%/~100% by design (memory project_get_status_has_no_reply, and the
    docstring this one previously carried, which claimed the field was
    excluded while the code below compared it anyway). It was redefined
    2026-09-10 (docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md,
    and see SafetyLinkStats.timeouts' own comment in devices_safety.py) to
    count ~500 ms poll iterations during which NO new STATUS frame was
    applied at all -- near zero on a healthy link, so a nonzero delta over a
    firing is real partial loss and belongs in this verdict."""
    deltas: "dict[str, Optional[int]]" = {}
    for key in ("crc_errors", "timeouts", "broadcast_dropped"):
        b, a = before.get(key), after.get(key)
        deltas[key] = None if (b is None or a is None) else (a - b)
    unknown = [k for k, d in deltas.items() if d is None]
    if len(unknown) == len(deltas):
        return CaseResult(
            Verdict.INCONCLUSIVE, reason="no link-stats fields were comparable", observed={"before": before, "after": after}
        )
    bad = {k: d for k, d in deltas.items() if d}
    if bad:
        return CaseResult(
            Verdict.FAIL,
            reason=f"nonzero delta(s) over the firing: {bad}",
            observed={"before": before, "after": after, "deltas": deltas},
        )
    return CaseResult(Verdict.PASS, observed={"before": before, "after": after, "deltas": deltas})


def judge_web_rw_toggle(field: str, original: Any, test_value: Any, write_ok: bool, after_write: Any,
                         restore_ok: bool, restored: Any) -> CaseResult:
    """WEB read/write round-trip cases (plan doc section 3.7, Wave 2):
    ``field`` was read, ``test_value`` written, read back, and ``original``
    restored -- in the case's own ``finally``, so this is called after the
    restore has already been attempted regardless of what happened above it.

    Verdict shape (task instruction): FAIL unconditionally if the restore
    itself did not round-trip -- leaving the board holding the test value is
    a hazard this suite must never paper over, no matter how the write path
    under test behaved. Only once the restore is confirmed does the verdict
    become "about the write path": did the POST actually take effect,
    confirmed by a real read-back rather than trusting the response body
    alone."""
    observed = {
        "field": field, "original": original, "test_value": test_value,
        "after_write": after_write, "restored": restored,
    }
    if not restore_ok or restored != original:
        return CaseResult(
            Verdict.FAIL,
            reason=(
                f"{field} restore did not round-trip: expected {original!r} back, "
                f"board now reads {restored!r} (restore POST ok={restore_ok})"
            ),
            observed=observed,
        )
    if not write_ok or after_write != test_value:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{field} write did not take effect: wrote {test_value!r}, board read back {after_write!r}",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_web_sec03(pw_ok: bool, enabled_ok: bool, dashboard_ok: Optional[bool],
                     admin_route_gated: Optional[bool], login_ok: bool, session_ok: bool,
                     extend_ok: bool, restore_ok: bool, restore_matches: bool,
                     state: Optional[dict] = None) -> CaseResult:
    """WEB-SEC-03: enable web auth with the harness credential, verify the
    auth surface (dashboard still open, an ADMIN-tier route now gated, login
    + session + extend all work), then disable and confirm the restore
    round-tripped. Same "restore failure always FAILs, otherwise the verdict
    is about the write path" shape as :func:`judge_web_rw_toggle`, just with
    more write-path steps to check in order -- the first one that did not
    hold names the reason."""
    observed = dict(state or {})
    if not restore_ok or not restore_matches:
        return CaseResult(
            Verdict.FAIL,
            reason="auth policy restore did not round-trip -- board may be left with web_enabled changed",
            observed=observed,
        )
    if not pw_ok:
        return CaseResult(
            Verdict.FAIL,
            reason="set_web_password did not confirm ok:true before enabling web auth -- refused to enable "
                   "against an unstored credential",
            observed=observed,
        )
    if not enabled_ok:
        return CaseResult(Verdict.FAIL, reason="set_policy(web_enabled=1) did not report ok:true", observed=observed)
    if not dashboard_ok:
        return CaseResult(
            Verdict.FAIL, reason="dashboard '/' did not answer 200 while web auth was enabled", observed=observed
        )
    if not admin_route_gated:
        return CaseResult(
            Verdict.FAIL,
            reason="/settings/zones was reachable with no session while web auth was enabled",
            observed=observed,
        )
    if not login_ok:
        return CaseResult(
            Verdict.FAIL, reason="POST /api/auth/login did not succeed with the harness credential", observed=observed
        )
    if not session_ok:
        return CaseResult(
            Verdict.FAIL, reason="GET /api/auth/session did not confirm the session after login", observed=observed
        )
    if not extend_ok:
        return CaseResult(
            Verdict.FAIL, reason="POST /api/auth/session/extend did not return 200", observed=observed
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_dual_reset_trip(
    link_up: bool,
    trip_reason: Optional[int],
    trip_mask: Optional[int],
    clear_ok: Optional[bool],
    readiness_trip_ok: Optional[bool],
) -> CaseResult:
    """OT-B01: sw_reset_esp(confirm=True) resets both processors. Link must
    come back up within the window with S6a (SAFETY_TRIP_MAIN_FAULT,
    trip_reason == 6) latched and NOTHING else -- trip_mask == 1 <<
    (trip_reason - 1) exactly, per link_frame_trip_mask_for_reason()
    (firmware/SaftyFW/src/tasks/link_frame.c:237). For reason 6 that is bit
    5, 0x0020 -- NOT 0x0040 (bit 6, reason 7 / SAFETY_TRIP_LINK_DEAD / S6b),
    the wrong constant a stale docstring elsewhere in this tree still names
    (CLAUDE.md correction). Any other bit or reason is FAIL. clear_ok and
    the readiness trip item must then confirm cleared."""
    observed = {
        "link_up": link_up, "trip_reason": trip_reason, "trip_mask": trip_mask,
        "clear_ok": clear_ok, "readiness_trip_ok": readiness_trip_ok,
    }
    if not link_up:
        return CaseResult(Verdict.FAIL, reason="safety link did not come back up within the window", observed=observed)
    if trip_reason != 6:
        return CaseResult(
            Verdict.FAIL, reason=f"trip_reason={trip_reason!r}, expected 6 (SAFETY_TRIP_MAIN_FAULT)",
            observed=observed, expected={"trip_reason": 6},
        )
    expected_mask = 1 << (trip_reason - 1)
    if trip_mask != expected_mask:
        return CaseResult(
            Verdict.FAIL,
            reason=(f"trip_mask=0x{(trip_mask or 0):04x}, expected 0x{expected_mask:04x} "
                    f"(1 << (trip_reason-1)) and nothing else"),
            observed=observed, expected={"trip_mask": expected_mask},
        )
    if not clear_ok:
        return CaseResult(Verdict.FAIL, reason="safety_clear_trip() did not confirm cleared once link+mask were verified", observed=observed)
    if readiness_trip_ok is False:
        return CaseResult(Verdict.FAIL, reason="/api/readiness trip item is not ok after clearing", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_push_applied(
    phase: Optional[str],
    running: Optional[str],
    expected_running: str,
    fw_build_matches_image: Optional[bool],
    fingerprint_identical: Optional[bool],
    boot_guard_recovery_mode: Optional[bool],
) -> CaseResult:
    """OT-E01: a good image pushed into app over Wi-Fi must reach phase ==
    "done", come back up RUNNING the expected partition, report the exact
    fw_build embedded in the pushed .bin's esp_app_desc_t, leave the
    zones-config fingerprint identical, and clear boot_guard's
    recovery_mode."""
    observed = {
        "phase": phase, "running": running, "expected_running": expected_running,
        "fw_build_matches_image": fw_build_matches_image,
        "fingerprint_identical": fingerprint_identical,
        "boot_guard_recovery_mode": boot_guard_recovery_mode,
    }
    if phase != "done":
        return CaseResult(Verdict.FAIL, reason=f"phase={phase!r}, expected 'done'", observed=observed)
    if running != expected_running:
        return CaseResult(
            Verdict.FAIL, reason=f"running={running!r}, expected {expected_running!r}",
            observed=observed, expected={"running": expected_running},
        )
    if fw_build_matches_image is not True:
        return CaseResult(Verdict.FAIL, reason="board's fw_build does not match the .bin's embedded build time", observed=observed)
    if fingerprint_identical is not True:
        return CaseResult(Verdict.FAIL, reason="zones-config fingerprint changed across the update", observed=observed)
    if boot_guard_recovery_mode is not False:
        return CaseResult(Verdict.FAIL, reason=f"boot_guard recovery_mode={boot_guard_recovery_mode!r}, expected False", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_push_refused(
    refused: Optional[bool],
    running_before: Optional[str],
    running_after: Optional[str],
    fw_build_before: Optional[str],
    fw_build_after: Optional[str],
) -> CaseResult:
    """OT-E03: a corrupt image must be refused (4xx, or phase:failed short
    of a reboot) -- fw_build and RUNNING must be exactly unchanged. A
    'refused' flag alone is not trusted: a refusal that nonetheless changed
    RUNNING or fw_build is still FAIL, meaning the board rebooted into the
    bad image before catching the problem."""
    observed = {
        "refused": refused, "running_before": running_before, "running_after": running_after,
        "fw_build_before": fw_build_before, "fw_build_after": fw_build_after,
    }
    if not refused:
        return CaseResult(Verdict.FAIL, reason="push was accepted; expected a refusal before reboot", observed=observed)
    if running_after != running_before:
        return CaseResult(
            Verdict.FAIL, reason=f"RUNNING partition changed ({running_before!r} -> {running_after!r}) despite refusal",
            observed=observed,
        )
    if fw_build_after != fw_build_before:
        return CaseResult(
            Verdict.FAIL, reason=f"fw_build changed ({fw_build_before!r} -> {fw_build_after!r}) despite refusal",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_rollback(
    fw_build_matches_pre_update: Optional[bool],
    pid_gains_before: "Optional[dict]",
    pid_gains_after: "Optional[dict]",
    zones_config_load_fault: Optional[bool],
) -> CaseResult:
    """OT-E02: the ZONES_CFG_VERSION rollback hazard -- rolling back past a
    zones-config schema bump makes the older firmware refuse the
    newer-than-it-knows blob and run on firmware-default PID gains,
    silently. A load_fault reported by /api/status after the rollback is
    FAIL by itself, and control_get_zones()'s PID gains must equal the
    pre-rollback values exactly -- a rollback that merely looks done (right
    fw_build, no load_fault) but changed gains is still FAIL."""
    observed = {
        "fw_build_matches_pre_update": fw_build_matches_pre_update,
        "pid_gains_before": pid_gains_before, "pid_gains_after": pid_gains_after,
        "zones_config_load_fault": zones_config_load_fault,
    }
    if zones_config_load_fault:
        return CaseResult(
            Verdict.FAIL,
            reason="zones_config_load_fault is true after rollback -- PID gains likely reset to firmware defaults (ZONES_CFG_VERSION hazard)",
            observed=observed,
        )
    if fw_build_matches_pre_update is not True:
        return CaseResult(Verdict.FAIL, reason="fw_build after rollback does not match the pre-update (pre-E01) value", observed=observed)
    if pid_gains_before is None or pid_gains_after is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="PID gains were not readable before or after the rollback", observed=observed)
    if pid_gains_before != pid_gains_after:
        return CaseResult(
            Verdict.FAIL, reason=f"PID gains changed by rollback: before={pid_gains_before} after={pid_gains_after}",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_power_loss_mid_write(
    fw_build_before: Optional[str],
    fw_build_after: Optional[str],
    running: Optional[str],
    expected_running: str,
    fingerprint_identical: Optional[bool],
) -> CaseResult:
    """OT-E06: power cut mid-write must leave the board exactly where it
    started -- the bootloader refuses the partial image, so ``fw_build``
    and RUNNING after power is restored must equal what they were before
    the push was ever attempted, same "unchanged" contract as
    ``judge_ota_push_refused`` (OT-E03/04/05) but observed across a real
    power cycle rather than an HTTP refusal. ``fingerprint_identical`` is
    None only when a zones read failed on one side -- that is INCONCLUSIVE,
    never a silent PASS, since the whole point of this case is confirming
    nothing changed."""
    observed = {
        "fw_build_before": fw_build_before, "fw_build_after": fw_build_after,
        "running": running, "expected_running": expected_running,
        "fingerprint_identical": fingerprint_identical,
    }
    if fw_build_after != fw_build_before:
        return CaseResult(
            Verdict.FAIL,
            reason=f"fw_build changed across a power-loss-mid-write ({fw_build_before!r} -> {fw_build_after!r}) -- bootloader accepted a partial image",
            observed=observed,
        )
    if running != expected_running:
        return CaseResult(
            Verdict.FAIL, reason=f"running={running!r}, expected {expected_running!r} after power restore",
            observed=observed,
        )
    if fingerprint_identical is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="zones-config fingerprint was not readable on one side of the power cut", observed=observed)
    if fingerprint_identical is not True:
        return CaseResult(Verdict.FAIL, reason="zones-config fingerprint changed across a power-loss-mid-write", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_update_refused_during_state(
    interlock_ok: Optional[bool],
    push_refused: Optional[bool],
    state_before: Optional[str],
    state_after: Optional[str],
    expected_state: str,
) -> CaseResult:
    """OT-E07/OT-E08: an OTA push attempted while a firing (OT-E07) or
    autotune (OT-E08) is active must be refused, and that firing/autotune
    must continue unaffected -- both the live GET /api/ota/interlock read
    and the push attempt itself must agree the update was blocked, and the
    state (``profiles_get_exec_status``/``autotune_get_status``) must read
    the same active value before and after the attempt, never having been
    disturbed by the refused push."""
    observed = {
        "interlock_ok": interlock_ok, "push_refused": push_refused,
        "state_before": state_before, "state_after": state_after, "expected_state": expected_state,
    }
    if state_before != expected_state:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"state_before={state_before!r}, expected {expected_state!r} -- the case's own precondition was not met",
            observed=observed,
        )
    if interlock_ok is not False:
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET /api/ota/interlock reported ok={interlock_ok!r} while {expected_state} was active, expected ok:false",
            observed=observed,
        )
    if not push_refused:
        return CaseResult(Verdict.FAIL, reason="OTA push was accepted while a firing/autotune was active, expected a refusal", observed=observed)
    if state_after != expected_state:
        return CaseResult(
            Verdict.FAIL,
            reason=f"state_after={state_after!r}, expected {expected_state!r} -- the refused push disturbed the run in progress",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_session_auth_tiers(admin_ok: Optional[bool], user_refused: Optional[bool]) -> CaseResult:
    """OT-E10: with web auth on, an ADMIN session cookie must be accepted
    for an OTA push (ota_http.c retires the AP-password HMAC entirely once
    ``http_auth_policy_web_enabled()`` is true -- the route-tier
    pre-handler already required ADMIN before the handler was reached) and
    a ``user``-tier session must be refused for the same route
    (`route_tier_table.h`'s ADMIN tier on ``/api/ota/esp``)."""
    observed = {"admin_ok": admin_ok, "user_refused": user_refused}
    if admin_ok is not True:
        return CaseResult(Verdict.FAIL, reason="OTA push with an ADMIN session cookie was refused, expected accepted", observed=observed)
    if not user_refused:
        return CaseResult(Verdict.FAIL, reason="OTA push with a user-tier session cookie was accepted, expected refused", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_partitions_state(running: Optional[str], expected_running: str) -> CaseResult:
    """OT-E12: otadata/partition state recorded after every OT-E case. Only
    FAILs the one condition the plan names: RUNNING reads back 'recovery'
    when the case expected 'app'."""
    observed = {"running": running, "expected_running": expected_running}
    if expected_running == "app" and running == "recovery":
        return CaseResult(
            Verdict.FAIL,
            reason="RUNNING partition is 'recovery' after a case that expected 'app' (otadata gap, CLAUDE.md flash section)",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


# ---------------------------------------------------------------------------
# OT-P* -- Pico OTA (plan doc section 3.4, Wave 4). Unlike the ESP (OT-E*),
# the Pico has no partition/fw_build HTTP surface (TODO.md 9.6: "most of it
# does NOT exist over this link") -- identity comes from the ESP's cached
# GET_FW_VERSION reply (safety_get_fw_version()'s describe() text: commit,
# boot_id) and boot-reason-not-watchdog comes from safety_get_diag()'s text,
# the same "parse the .describe()/.text report" convention SP-08/SP-09
# (cases_safety.py) already use for trip_reason/trip_mask.
# ---------------------------------------------------------------------------

def parse_fw_version_commit(fw_version_text: str) -> "str | None":
    """Pulls the commit hash out of SafetyFwVersion.describe()'s
    "Pico build: <commit>[ (dirty)] built <time>, boot_id=..." line. None
    for the "unknown (Pico has not reported a build identity)" case --
    never an empty string standing in for "no answer", matching
    SafetyFwVersion's own "empty commit is unknown" convention."""
    if "unknown (Pico has not reported" in fw_version_text:
        return None
    m = re.search(r"Pico build:\s*(\S+?)(?:\s*\(dirty\))?\s+built\b", fw_version_text)
    return m.group(1) if m else None


def parse_fw_version_boot_id(fw_version_text: str) -> "int | None":
    m = re.search(r"boot_id\s*=\s*(\d+)", fw_version_text)
    return int(m.group(1)) if m else None


def parse_diag_boot_reason(diag_text: str) -> "str | None":
    """SP/OT-P shared: whatever token SafetyDiag.describe() prints for its
    boot reason field. None if the diag text carries no such field at all
    (older firmware / field not populated) -- distinct from finding one
    that happens to read "watchdog"."""
    m = re.search(r"boot_reason\s*[:=]?\s*(\w+)", diag_text)
    return m.group(1) if m else None


def judge_ota_pico_push_applied(
    phase: Optional[str],
    commit_after: "Optional[str]",
    expected_commit: "Optional[str]",
    boot_reason: "Optional[str]",
    commissioning_identical: Optional[bool],
) -> CaseResult:
    """OT-P01: a Pico image relayed into the inactive slot must reach
    ``phase == "done"``, come back up reporting the pushed image's commit
    via GET_FW_VERSION, never report a watchdog boot reason (the
    2026-09-18 erase-time watchdog defect's signature -- see OT-P04's own
    judge for the sharper, defect-specific version of this same check),
    and leave the commissioning config byte-identical."""
    observed = {
        "phase": phase, "commit_after": commit_after, "expected_commit": expected_commit,
        "boot_reason": boot_reason, "commissioning_identical": commissioning_identical,
    }
    if phase != "done":
        return CaseResult(Verdict.FAIL, reason=f"phase={phase!r}, expected 'done'", observed=observed)
    if boot_reason == "watchdog":
        return CaseResult(
            Verdict.FAIL,
            reason="Pico boot_reason is 'watchdog' after the relay (2026-09-18 erase-time watchdog defect's signature)",
            observed=observed,
        )
    if expected_commit and commit_after != expected_commit:
        return CaseResult(
            Verdict.FAIL,
            reason=f"Pico commit after relay is {commit_after!r}, expected {expected_commit!r}",
            observed=observed,
        )
    if commissioning_identical is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="commissioning config was not readable before or after the relay", observed=observed)
    if commissioning_identical is not True:
        return CaseResult(Verdict.FAIL, reason="commissioning config changed across the Pico relay", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_pico_rollback(
    rollback_status: Optional[str],
    commit_after: "Optional[str]",
    expected_previous_commit: "Optional[str]",
    commissioning_identical: Optional[bool],
) -> CaseResult:
    """OT-P02: after OT-P01 boots the new slot, a rollback must report
    ``status == "rebooting"`` (the only success outcome
    ota_http_pico_rollback_format_body() defines -- every other status,
    including "unknown", is not a confirmed rollback) and come back
    running the pre-P01 commit, with commissioning config unchanged."""
    observed = {
        "rollback_status": rollback_status, "commit_after": commit_after,
        "expected_previous_commit": expected_previous_commit,
        "commissioning_identical": commissioning_identical,
    }
    if rollback_status != "rebooting":
        return CaseResult(
            Verdict.FAIL,
            reason=f"rollback status={rollback_status!r}, expected 'rebooting' (a confirmed reboot into a different image)",
            observed=observed,
        )
    if expected_previous_commit and commit_after != expected_previous_commit:
        return CaseResult(
            Verdict.FAIL,
            reason=f"commit after rollback is {commit_after!r}, expected the pre-update commit {expected_previous_commit!r}",
            observed=observed,
        )
    if commissioning_identical is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="commissioning config was not readable before or after the rollback", observed=observed)
    if commissioning_identical is not True:
        return CaseResult(Verdict.FAIL, reason="commissioning config changed across the Pico rollback", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_pico_bad_image_fallback(
    push_refused_or_failed: Optional[bool],
    commit_before: "Optional[str]",
    commit_after: "Optional[str]",
    boot_reason: "Optional[str]",
) -> CaseResult:
    """OT-P03: a CRC-corrupted Pico image must be refused by the relay
    (push_pico_image() raises/refuses) or fail the staged CRC check
    (phase: failed) before the bootloader ever switches slots -- the
    running commit must be exactly unchanged, and boot_reason must never
    read 'watchdog' (a corrupt-image attempt must not itself trip the
    2026-09-18 erase-time defect)."""
    observed = {
        "push_refused_or_failed": push_refused_or_failed,
        "commit_before": commit_before, "commit_after": commit_after, "boot_reason": boot_reason,
    }
    if not push_refused_or_failed:
        return CaseResult(Verdict.FAIL, reason="corrupt Pico image was accepted; expected a refusal/failed relay", observed=observed)
    if boot_reason == "watchdog":
        return CaseResult(Verdict.FAIL, reason="Pico boot_reason is 'watchdog' after a corrupt-image attempt", observed=observed)
    if commit_before is not None and commit_after != commit_before:
        return CaseResult(
            Verdict.FAIL,
            reason=f"Pico commit changed ({commit_before!r} -> {commit_after!r}) despite the corrupt image being refused",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_pico_no_watchdog_signature(
    boot_reason: "Optional[str]",
    last_error: "Optional[str]",
) -> CaseResult:
    """OT-P04: judges OT-P01's own relay specifically for the 2026-09-18
    erase-time watchdog defect's two documented symptoms -- boot_reason
    must never be 'watchdog', and get_pico_status()'s last_error must
    never contain the defect's exact signature string, "did not confirm
    RECEIVING". Deliberately narrower and more literal than OT-P01's own
    general judge, so a regression of this SPECIFIC defect is legible even
    if OT-P01 as a whole still happens to read PASS for some other reason."""
    observed = {"boot_reason": boot_reason, "last_error": last_error}
    if boot_reason == "watchdog":
        return CaseResult(Verdict.FAIL, reason="Pico boot_reason is 'watchdog' (2026-09-18 erase-time watchdog defect)", observed=observed)
    if last_error and "did not confirm RECEIVING" in last_error:
        return CaseResult(
            Verdict.FAIL,
            reason=f"last_error carries the 2026-09-18 defect's exact signature: {last_error!r}",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_ota_pico_refused_with_trip_pending(
    trip_pending: Optional[bool],
    push_refused: Optional[bool],
    commit_before: "Optional[str]",
    commit_after: "Optional[str]",
) -> CaseResult:
    """OT-P05: a Pico update attempted while a real trip is latched must be
    refused (UPDATE_PROTOCOL.md's hardware exercise) with the running
    commit left exactly unchanged -- same "refused means truly untouched"
    discipline as judge_ota_push_refused (OT-E03)."""
    observed = {
        "trip_pending": trip_pending, "push_refused": push_refused,
        "commit_before": commit_before, "commit_after": commit_after,
    }
    if trip_pending is not True:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no trip was actually pending at push time -- the case's own precondition was not met",
            observed=observed,
        )
    if not push_refused:
        return CaseResult(Verdict.FAIL, reason="Pico update was accepted while a trip was pending, expected a refusal", observed=observed)
    if commit_before is not None and commit_after != commit_before:
        return CaseResult(
            Verdict.FAIL,
            reason=f"Pico commit changed ({commit_before!r} -> {commit_after!r}) despite the refused push",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


# ---------------------------------------------------------------------------
# AT-* -- autotune on the 4W fixture (plan doc section 3.5, Wave 3 part A)
# ---------------------------------------------------------------------------

def judge_autotune_fit(
    method: str,
    model_valid: bool,
    baseline_c: Optional[float],
    ambient_ref: float,
    k_gain_c_per_duty: Optional[float],
    tau_s: Optional[float],
    max_temp_c: Optional[float],
    tripped: bool,
    relay_valid: bool = True,
    relay_amplitude_c: Optional[float] = None,
    min_relay_amplitude_c: float = 2.0,
    rest_band_c: float = 2.0,
    k_expected_c_per_duty: float = 38.0,
    k_tol_fraction: float = 0.15,
    tau_expected_s: float = 265.0,
    tau_tol_fraction: float = 0.25,
    max_temp_limit_c: float = 70.0,
) -> CaseResult:
    """AT-01 (method='step') / AT-04 (method='relay'): the run reaches a
    fitted result; ``baseline_c`` within ``rest_band_c`` of the rested
    ambient reference; fitted ``K`` within +-15% of the last committed
    bench value (~38 C/duty, memory project_bench_is_a_4w_test_fixture);
    ``tau`` within +-25% of ~265s; no trip; max temperature stayed under
    70 C. AT-04's relay method gets one extra, plan-mandated escape hatch:
    if the relay result is invalid *because the fixture could not sustain
    the oscillation amplitude*, that is INCONCLUSIVE (with the amplitude
    recorded), never a FAIL -- a real, expected limit of a ~4W fixture, not
    a defect. Any other invalid-relay-result cause is still a FAIL."""
    observed = {
        "baseline_c": baseline_c, "ambient_ref": ambient_ref, "k_gain_c_per_duty": k_gain_c_per_duty,
        "tau_s": tau_s, "max_temp_c": max_temp_c, "tripped": tripped,
    }
    if tripped:
        return CaseResult(Verdict.FAIL, reason="a safety trip occurred during the run", observed=observed)
    if max_temp_c is not None and max_temp_c >= max_temp_limit_c:
        return CaseResult(
            Verdict.FAIL,
            reason=f"max temperature {max_temp_c:.1f}C reached/exceeded the {max_temp_limit_c:.0f}C limit",
            observed=observed,
        )
    if method == "relay" and not relay_valid:
        if relay_amplitude_c is not None and relay_amplitude_c < min_relay_amplitude_c:
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=(
                    f"fixture could not sustain the oscillation amplitude "
                    f"({relay_amplitude_c:.2f}C < {min_relay_amplitude_c:.2f}C)"
                ),
                observed={**observed, "relay_amplitude_c": relay_amplitude_c},
            )
        return CaseResult(
            Verdict.FAIL, reason="relay result invalid for a reason other than insufficient amplitude",
            observed={**observed, "relay_amplitude_c": relay_amplitude_c},
        )
    if not model_valid:
        return CaseResult(Verdict.FAIL, reason="no fitted model was produced (model_valid is false)", observed=observed)
    if baseline_c is None:
        return CaseResult(Verdict.FAIL, reason="no valid baseline temperature reading", observed=observed)
    if abs(baseline_c - ambient_ref) > rest_band_c:
        return CaseResult(
            Verdict.FAIL,
            reason=f"baseline {baseline_c:.1f}C is more than {rest_band_c:.1f}C from the rested reference {ambient_ref:.1f}C",
            observed=observed,
        )
    if k_gain_c_per_duty is None or abs(k_gain_c_per_duty - k_expected_c_per_duty) > k_tol_fraction * k_expected_c_per_duty:
        return CaseResult(
            Verdict.FAIL,
            reason=f"fitted K={k_gain_c_per_duty!r} outside +-{k_tol_fraction*100:.0f}% of {k_expected_c_per_duty:.1f}C/duty",
            observed=observed, expected={"k_expected_c_per_duty": k_expected_c_per_duty, "k_tol_fraction": k_tol_fraction},
        )
    if tau_s is None or abs(tau_s - tau_expected_s) > tau_tol_fraction * tau_expected_s:
        return CaseResult(
            Verdict.FAIL,
            reason=f"fitted tau={tau_s!r} outside +-{tau_tol_fraction*100:.0f}% of {tau_expected_s:.0f}s",
            observed=observed, expected={"tau_expected_s": tau_expected_s, "tau_tol_fraction": tau_tol_fraction},
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_autotune_abort_immediate(
    state_name: str, duties: "list[float]", relays_off: Optional[bool], elapsed_since_abort_s: float,
    timeout_s: float = 5.0,
) -> CaseResult:
    """AT-02: status goes idle within 5s of ``autotune_abort()``, every zone
    duty reads 0 within one control tick, and ``io_read()`` shows the
    heater relays off. ``relays_off=None`` (the read failed / no host) is
    reported INCONCLUSIVE for that half of the check rather than a silent
    PASS, matching ``judge_relay_energized``'s discipline elsewhere in this
    module."""
    observed = {"state_name": state_name, "duties": duties, "relays_off": relays_off, "elapsed_since_abort_s": elapsed_since_abort_s}
    if elapsed_since_abort_s > timeout_s:
        return CaseResult(
            Verdict.FAIL,
            reason=f"status did not go idle within {timeout_s:.0f}s of autotune_abort() (took {elapsed_since_abort_s:.1f}s)",
            observed=observed,
        )
    if state_name != "idle":
        return CaseResult(Verdict.FAIL, reason=f"state is {state_name!r}, expected 'idle' after abort", observed=observed)
    nonzero = [d for d in duties if d]
    if nonzero:
        return CaseResult(Verdict.FAIL, reason=f"{len(nonzero)} zone(s) had nonzero duty after abort", observed=observed)
    if relays_off is None:
        return CaseResult(
            Verdict.INCONCLUSIVE, reason="could not read io_read() to confirm heater relays are off", observed=observed
        )
    if not relays_off:
        return CaseResult(Verdict.FAIL, reason="io_read() shows a heater relay still on after abort", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_autotune_accept_guarded(
    accept_ok: bool, accept_reason: str, gains_before: tuple, gains_after: tuple,
) -> CaseResult:
    """AT-03: ``autotune_accept()`` (no ``ack_unsettled``) is refused on an
    unsettled fit, and ``control_get_zones``' gains are unchanged either
    way -- the guard must be all-or-nothing, never "refused but wrote
    anyway"."""
    observed = {"accept_ok": accept_ok, "accept_reason": accept_reason, "gains_before": gains_before, "gains_after": gains_after}
    if accept_ok:
        return CaseResult(
            Verdict.FAIL, reason="autotune_accept() succeeded on an unsettled fit; expected a refusal", observed=observed
        )
    if gains_before != gains_after:
        return CaseResult(
            Verdict.FAIL, reason="zone PID gains changed despite the accept being refused", observed=observed
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_autotune_matrix(matrix: Any, zone_row: int = 0) -> CaseResult:
    """AT-05: ``GET /api/autotune/matrix`` is a well-formed 3x3 with the
    given zone's row populated (three finite numeric entries -- coupling
    onto itself and both neighbors)."""
    if not isinstance(matrix, list) or len(matrix) != 3 or any(not isinstance(row, list) or len(row) != 3 for row in matrix):
        return CaseResult(Verdict.FAIL, reason="matrix is not well-formed 3x3", observed={"matrix": matrix})
    row = matrix[zone_row]
    bad = [v for v in row if isinstance(v, bool) or not isinstance(v, (int, float))]
    if bad:
        return CaseResult(
            Verdict.FAIL, reason=f"zone {zone_row}'s row is not fully populated with numbers: {row!r}",
            observed={"matrix": matrix},
        )
    return CaseResult(Verdict.PASS, observed={"matrix": matrix, "row": row})


# ---------------------------------------------------------------------------
# HP-03 / HP-07 -- on/off zone cycling and a provoked software fault
# (plan doc section 3.6, Wave 3 part A)
# ---------------------------------------------------------------------------

def judge_on_off_zone_cycling(
    relay_states: "list[bool]",
    temps_c: "list[Optional[float]]",
    target_c: float,
    hyst_c: float,
    min_transitions: int = 2,
    margin_c: float = 3.0,
) -> CaseResult:
    """HP-03: an on/off-type zone's relay toggles at least ``min_transitions``
    times over the run (proof it is actually cycling, not stuck on or off),
    and every sampled temperature stays within ``hyst_c + margin_c`` of
    ``target_c`` -- the margin is slack for real thermal lag/overshoot on a
    hysteresis controller, not a second hysteresis band to tune against."""
    observed = {"relay_states": relay_states, "temps_c": temps_c, "target_c": target_c, "hyst_c": hyst_c}
    if len(relay_states) < 2:
        return CaseResult(Verdict.INCONCLUSIVE, reason="fewer than 2 relay samples collected", observed=observed)
    transitions = sum(1 for a, b in zip(relay_states, relay_states[1:]) if a != b)
    if transitions < min_transitions:
        return CaseResult(
            Verdict.FAIL,
            reason=f"relay only toggled {transitions} time(s), expected >= {min_transitions}",
            observed={**observed, "transitions": transitions},
        )
    band = hyst_c + margin_c
    out_of_band = [t for t in temps_c if t is not None and abs(t - target_c) > band]
    if out_of_band:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(out_of_band)} sample(s) strayed more than {band:.1f}C from target {target_c:.1f}C",
            observed={**observed, "out_of_band": out_of_band},
        )
    return CaseResult(Verdict.PASS, observed={**observed, "transitions": transitions})


#: firmware/KilnFW/App/drivers/http/app.js's THERMAL_GUARD_WORDS -- 5 is
#: "over absolute max temperature", the only guard HP-07 deliberately
#: provokes (a software max_temp_c ceiling set just above ambient, never a
#: Pico/hardware trip). Kept here as the one named constant rather than a
#: magic number in the judge below.
THERMAL_GUARD_OVER_MAX_TEMP = 5


def judge_faulted_run(
    state_name: str,
    faulted: bool,
    fault_guard: Optional[int],
    ack_ok: bool,
    post_ack_state: str,
    expected_guard: int = THERMAL_GUARD_OVER_MAX_TEMP,
) -> CaseResult:
    """HP-07: a profile whose zone max_temp_c is set 3C above ambient
    reaches FAULTED with fault_guard naming the over-max-temp guard (not a
    Pico/safety trip), and the sticky bar's Acknowledge
    (POST /api/profile_exec/stop) clears it back to a non-faulted state."""
    observed = {
        "state_name": state_name, "faulted": faulted, "fault_guard": fault_guard,
        "ack_ok": ack_ok, "post_ack_state": post_ack_state,
    }
    if state_name != "faulted" or not faulted:
        return CaseResult(
            Verdict.FAIL, reason=f"expected state=faulted/faulted=True, got state={state_name!r} faulted={faulted}",
            observed=observed,
        )
    if fault_guard != expected_guard:
        return CaseResult(
            Verdict.FAIL,
            reason=f"fault_guard={fault_guard!r}, expected {expected_guard} (over absolute max temperature)",
            observed=observed,
        )
    if not ack_ok:
        return CaseResult(Verdict.FAIL, reason="Acknowledge (profiles.stop) was refused on a faulted run", observed=observed)
    if post_ack_state == "faulted":
        return CaseResult(Verdict.FAIL, reason="state is still 'faulted' after Acknowledge", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


#: Both fields live in `safety_get_diag()`'s text (SafetyDiag.describe:
#: "trip_reason 6 [...] | warn_mask 0x0000 | trip_mask 0x0020"), NEVER in
#: `safety_get_status()`'s text -- SafetyStatus (devices_safety.py) carries
#: neither field, so parsing either one off the status report can only ever
#: report "not found" (never a real, possibly-nonzero value). Shared here
#: (rather than duplicated per case module) so SP-02 and SP-08/SP-09 read
#: the exact same vocabulary the device module itself emits, not an
#: invented one only a test fixture would produce. The separators are
#: spaces in the real text, so accept `:`/`=`/whitespace alike.
def parse_trip_reason(diag_text: str) -> "int | None":
    m = re.search(r"trip_reason\s*[:=]?\s*(\d+)", diag_text)
    return int(m.group(1)) if m else None


def parse_trip_mask(diag_text: str) -> "int | None":
    m = re.search(r"trip_mask\s*[:=]?\s*(0x[0-9a-fA-F]+|\d+)", diag_text)
    if not m:
        return None
    return int(m.group(1), 0)


def safety_trip_mask_for_reason(trip_reason: int) -> int:
    """`trip_mask = 1 << (trip_reason - 1)`, 0 for trip_reason 0 (NONE) --
    mirrors link_frame_trip_mask_for_reason() (firmware/SaftyFW/src/tasks/
    link_frame.c) and devices_safety.py's own helper of the same name.
    Duplicated here (rather than imported) so this pure-judgment module
    keeps zero dependency on the live-device client modules -- see this
    file's docstring on why every judge function here is import-light and
    I/O-free."""
    if not trip_reason:
        return 0
    return 1 << (trip_reason - 1)


def judge_operator_trip(
    trip_reason: Optional[int],
    expected_reason: int,
    cleared_after: Optional[bool],
    trip_mask: Optional[int] = None,
) -> CaseResult:
    """SP-08/SP-09 shared shape: an operator-induced trip must show up as
    EXACTLY the expected `trip_reason` (never a different or additional
    guard also latched) and must clear when asked.

    `expected_reason` is the plan's own SP-08 (S7, ESTOP=8) / SP-09 (S6b,
    LINK_DEAD=7) value -- callers pass the specific one their case is
    checking, never a guessed default, per CLAUDE.md's warning that bit
    position is NOT the guard number past S3."""
    if trip_reason is None:
        return CaseResult(Verdict.FAIL, reason="no trip_reason reported after the operator action", observed={})
    if trip_reason != expected_reason:
        expected_mask = safety_trip_mask_for_reason(expected_reason)
        observed_mask = safety_trip_mask_for_reason(trip_reason)
        return CaseResult(
            Verdict.FAIL,
            reason=(
                f"trip_reason={trip_reason} (mask {observed_mask:#04x}) does not match the expected "
                f"trip_reason={expected_reason} (mask {expected_mask:#04x})"
            ),
            observed={"trip_reason": trip_reason, "expected_reason": expected_reason},
        )
    expected_mask = safety_trip_mask_for_reason(expected_reason)
    if trip_mask is not None and trip_mask != expected_mask:
        # Plan section 6 rule 5: the reason ALONE matching is not enough --
        # a second guard latched at the same time shows up only in the
        # mask, and clearing on a reason match alone would wipe it
        # unseen. The caller must not have cleared in that case either.
        return CaseResult(
            Verdict.FAIL,
            reason=(
                f"trip_reason={trip_reason} matched but trip_mask={trip_mask:#06x} is not exactly "
                f"the expected {expected_mask:#06x} -- another guard is latched too"
            ),
            observed={"trip_reason": trip_reason, "trip_mask": trip_mask, "expected_mask": expected_mask},
        )
    if cleared_after is False:
        return CaseResult(
            Verdict.FAIL,
            reason=f"trip_reason={trip_reason} latched correctly but did not clear after safety_clear_trip()",
            observed={"trip_reason": trip_reason, "cleared_after": cleared_after},
        )
    if cleared_after is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="trip latched correctly but the post-clear state could not be read back",
            observed={"trip_reason": trip_reason},
        )
    return CaseResult(
        Verdict.PASS,
        observed={"trip_reason": trip_reason, "trip_mask": trip_mask, "cleared_after": cleared_after},
    )


def judge_wifi_mode_returned_home(
    mode_during_ap: Optional[str],
    mode_after: Optional[str],
    operator_confirmed_ap_seen: Optional[bool],
) -> CaseResult:
    """WEB-WIFI-06: toggling AP mode on must actually report `mode="ap"`
    while it's on (or the operator must confirm they saw the AP network --
    the board's own status read and the operator's eyes are independent
    checks, either is enough), and the board must return to `mode="home"`
    afterward. Never trusts the operator's yes/no alone without ALSO
    checking that going back to "home" actually took, since a stuck-in-AP
    board is a real regression an operator's "yes I saw it" would not
    catch."""
    if operator_confirmed_ap_seen is False and mode_during_ap != "ap":
        return CaseResult(
            Verdict.FAIL,
            reason="operator did not observe the AP network and the board never reported mode=ap",
            observed={"mode_during_ap": mode_during_ap, "operator_confirmed_ap_seen": operator_confirmed_ap_seen},
        )
    if mode_after != "home":
        return CaseResult(
            Verdict.FAIL,
            reason=f"board did not return to mode=home afterward (reported mode={mode_after!r})",
            observed={"mode_after": mode_after},
        )
    return CaseResult(
        Verdict.PASS,
        observed={
            "mode_during_ap": mode_during_ap,
            "mode_after": mode_after,
            "operator_confirmed_ap_seen": operator_confirmed_ap_seen,
        },
    )


def judge_flash_round_trip(
    verified: Optional[bool],
    running_partition: Optional[str],
    expected_partition: str,
    error: Optional[str],
) -> CaseResult:
    """FL-10/FL-11: a JTAG flash round trip is only a PASS when the tool's
    own post-flash verification (flash_firmware()'s `verify=True`, or the
    S6a-clear procedure's link-up check for FL-11) reported success AND the
    board is confirmed running the expected partition/side. Any refusal or
    exception from the flashing tool itself is a FAIL, never a lesser
    verdict (plan section 6 rule 3: never treat a refusal as anything but a
    FAIL) -- callers pass that in as `error`."""
    if error:
        return CaseResult(Verdict.FAIL, reason=f"flashing tool reported an error: {error}", observed={"error": error})
    if verified is False:
        return CaseResult(
            Verdict.FAIL,
            reason=f"post-flash verification failed (running partition={running_partition!r}, expected {expected_partition!r})",
            observed={"running_partition": running_partition, "expected_partition": expected_partition},
        )
    if verified is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="flash completed but verification result could not be determined",
            observed={"running_partition": running_partition},
        )
    if running_partition is not None and running_partition != expected_partition:
        return CaseResult(
            Verdict.FAIL,
            reason=f"verified=True but running partition {running_partition!r} != expected {expected_partition!r}",
            observed={"running_partition": running_partition, "expected_partition": expected_partition},
        )
    return CaseResult(Verdict.PASS, observed={"running_partition": running_partition, "verified": verified})


def judge_login_lockout(status_codes: "list[Optional[int]]") -> CaseResult:
    """WEB-SEC-05: 6 deliberately-wrong logins against /api/auth/login must
    eventually draw a 429 (memory project_login_lockout_saturation_accepted:
    ~1 request per 19s denies login to any new address afterward, which is
    why this case must run dead last -- see registry._ALWAYS_LAST). A run
    that never sees a 429 across all attempts is a real regression: either
    the lockout stopped engaging, or every attempt unexpectedly succeeded
    (401 the whole way through is also wrong -- the endpoint should have
    started refusing outright)."""
    if not status_codes:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no login attempts were made", observed={})
    if 429 in status_codes:
        return CaseResult(Verdict.PASS, observed={"status_codes": status_codes})
    if 200 in status_codes:
        return CaseResult(
            Verdict.FAIL,
            reason="a deliberately-wrong-password login attempt succeeded (200) -- credentials or lockout logic broken",
            observed={"status_codes": status_codes},
        )
    return CaseResult(
        Verdict.FAIL,
        reason=f"no 429 seen across {len(status_codes)} bad-login attempts -- lockout did not engage",
        observed={"status_codes": status_codes},
    )
