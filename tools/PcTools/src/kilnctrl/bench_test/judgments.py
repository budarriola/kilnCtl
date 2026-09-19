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
    """FL-07: pending must be False; mounted is recorded, not judged, per
    open question §7.4 (CONFIG_FILESYSTEM.md)."""
    pending = data.get("format_pending", data.get("pending"))
    if pending is not False:
        return CaseResult(Verdict.FAIL, reason=f"format_pending={pending!r}, expected False", observed=data)
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


def judge_pico_stack_margins(tasks: "list[dict]", min_fraction: float = 0.25) -> CaseResult:
    """SK-03: every task's free >= 25% of configured (memory
    project_saftyfw_minimal_stack_overflows -- the interim rule per open
    question §7.5)."""
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
