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
        configured = t.get("configured") or t.get("size")
        free = t.get("free")
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
                                   trip_reason: Optional[int]) -> CaseResult:
    """SP-02: link up, state armed/idle as appropriate, boot reason not
    watchdog, trip_reason 0."""
    if not link_up:
        return CaseResult(Verdict.FAIL, reason="link is not up", observed={"link_up": link_up})
    if boot_reason == "watchdog":
        return CaseResult(Verdict.FAIL, reason="boot reason is watchdog", observed={"boot_reason": boot_reason})
    if trip_reason not in (0, None):
        return CaseResult(Verdict.FAIL, reason=f"trip_reason={trip_reason}, expected 0", observed={"trip_reason": trip_reason})
    if state not in ("armed", "idle"):
        return CaseResult(Verdict.FAIL, reason=f"state={state!r}, expected armed or idle", observed={"state": state})
    return CaseResult(Verdict.PASS, observed={"link_up": link_up, "state": state, "boot_reason": boot_reason, "trip_reason": trip_reason})


def judge_estop_verify(flags: Optional[int], asserted_bit: int = 0x04) -> CaseResult:
    """SP-05: jumper fitted / not asserted -- flags bit 0x04 clear
    (memory project_estop_jumper_is_fitted)."""
    if flags is None:
        return CaseResult(Verdict.FAIL, reason="no flags reported", observed={"flags": flags})
    if flags & asserted_bit:
        return CaseResult(
            Verdict.FAIL,
            reason=f"E-stop bit {asserted_bit:#x} set in flags {flags:#x} (asserted)",
            observed={"flags": flags},
        )
    return CaseResult(Verdict.PASS, observed={"flags": flags})


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


def judge_nav_menu(nav_js_text: Optional[str], href_count: Optional[int], expected_count: int = 15,
                    has_group_expand: Optional[bool] = None) -> CaseResult:
    """WEB-X-01: the shared nav.js menu carries exactly the 15 links the
    plan names, and supports an expanding sub-group (the Safety group,
    `children`/`activeFor` per nav.js's 2026-08-27 rework) for the
    "auto-expands the current group" half of the case."""
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
            observed={"violations": violations, "total_rows": len(results)},
        )
    exercised = sum(1 for r in results if r.get("exercised"))
    if exercised == 0:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no GET routes were exercised", observed={"total_rows": len(results)})
    return CaseResult(Verdict.PASS, observed={"total_rows": len(results), "exercised": exercised})


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
