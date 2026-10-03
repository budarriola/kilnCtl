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
from typing import Any, Iterable, List, Optional

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


def judge_pico_slot_metadata(commit: Optional[str], boot_reason: Optional[str],
                              boot_loop_corroborated: Optional[bool] = None) -> CaseResult:
    """FL-08: commit is a real hash; boot reason is power_on/sw_reset, not
    watchdog.

    A lone ``boot_reason == "watchdog"`` is downgraded to INCONCLUSIVE, not
    FAIL: OpenOCD's rp2040 SWD reset path (used by every
    ``debug_program(peer="pico")``) itself reboots the RP2040 through its
    own watchdog -- observed directly, not inferred:
    ``docs/audits/short_proof_run_completed_2026-09-09.md``:33-36 records a
    deliberate SWD reset reporting ``boot reason: watchdog``, not power_on or
    a real watchdog event -- so a board that was simply
    debug-reset a moment ago reads identically to one boot-looping on a real
    watchdog timeout -- "watchdog" alone carries no information about which
    happened. ``boot_loop_corroborated``, when the caller can supply it
    (e.g. boot_id churn observed between two reads in the same run, or a
    required task reading DEAD), escalates a watchdog reading to a real
    FAIL; leaving it None/False keeps this case honest about not knowing
    which case it is, rather than crying wolf on every routine debug reset."""
    if not commit or not re.match(r"^[0-9a-fA-F]{6,40}$", commit):
        return CaseResult(Verdict.FAIL, reason=f"commit {commit!r} is not a real hash", observed={"commit": commit})
    if boot_reason == "watchdog":
        if boot_loop_corroborated:
            return CaseResult(
                Verdict.FAIL,
                reason="boot reason is watchdog, corroborated by a second boot-loop signal",
                observed={"boot_reason": boot_reason, "boot_loop_corroborated": boot_loop_corroborated},
            )
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(
                "boot reason is watchdog, but OpenOCD's SWD reset path also reboots the "
                "rp2040 via its own watchdog (docs/audits/short_proof_run_completed_"
                "2026-09-09.md:33-36) -- indistinguishable from a real boot loop "
                "without corroboration, which was not supplied"
            ),
            observed={"boot_reason": boot_reason, "commit": commit},
        )
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


#: SK-02's live 2026-09-24 bench run FAILed on 4-8 B drops against baseline
#: on tasks nobody touched -- ordinary run-to-run noise (compiler frame-
#: layout shifts from unrelated code elsewhere in the same translation unit,
#: and path differences between two runs of the same task, e.g. which branch
#: of an if/else happened to run deepest) rather than a real regression.
#: This tolerance absorbs that noise; a drop bigger than this many bytes is
#: a hard FAIL on a same, known commit (never silently masked -- see the
#: negative test in test_bench_test_wave1d.py), but only ever INCONCLUSIVE
#: across a commit mismatch or an unverified commit -- a different build
#: legitimately differs, so a bigger-than-tolerance drop there is not
#: evidence of a regression, just evidence the comparison isn't
#: apples-to-apples. A task under its configured absolute floor
#: (``min_free_bytes``) still FAILs regardless of this tolerance or any
#: commit mismatch.
_STACK_MARGIN_NOISE_TOLERANCE_BYTES = 64

#: Per-task SK-01/SK-02 tolerance overrides (owner decision 2026-10-01).
#: The three UART bridge tasks sit at a pristine HWM until their first
#: command, and the first command lowers it by about 1 KB (confirmed for
#: thermo_uart_bridge/io_uart_bridge: first thermo_read/io_read per boot).
#: The warmup rule absorbs that step, but boot-to-boot spread remains:
#: thermo_uart_bridge read 1660 B free on one boot and 1856 B on another
#: (196 B apart on the same commit), and info_uart_bridge showed a 176 B
#: drop that no single info command reproduces (ESP_LOGW formatting through
#: uart_log_vprintf under log-queue pressure is the leading candidate). A
#: 64 B tolerance flags that as a regression every other boot, so these
#: three get 384 B; every other task keeps the default. Only the tolerance
#: moves: the absolute floor and the dead-task FAIL are untouched.
_STACK_MARGIN_TASK_TOLERANCE_OVERRIDES = {
    "thermo_uart_bridge": 384,
    "io_uart_bridge": 384,
    "info_uart_bridge": 384,
}

#: Commit strings that mean "the build did not report one" (info.py /
#: stack_margin_baseline.py placeholders) -- treated as unknown, never as a
#: match, by judge_stack_margin_against_baseline().
_UNKNOWN_FW_COMMITS = frozenset({"", "?", "unknown"})


def judge_stack_margin_against_baseline(
    entries, baseline_by_name: dict, min_free_bytes: Optional[int] = None,
    board_fw_commit: Optional[str] = None,
    baseline_fw_commits: "Optional[Iterable[str]]" = None,
    tolerance_bytes: int = _STACK_MARGIN_NOISE_TOLERANCE_BYTES,
    task_tolerance_overrides: "Optional[dict]" = None,
) -> CaseResult:
    """SK-01/SK-02 (wave 1d): compares a live ``StackMarginEntry`` reading
    against the committed baseline's worst-case-across-conditions figure
    per task name (``stack_margin_baseline.worst_case_across_conditions()``).

    Per plan §3.3's note: a task with no baseline record at all is
    INCONCLUSIVE for that task (not FAIL -- there is nothing to compare
    against yet) rather than sinking the whole case; a task that regressed
    below its own committed worst case by more than ``tolerance_bytes``
    (see :data:`_STACK_MARGIN_NOISE_TOLERANCE_BYTES`) is FAIL. ``min_free_bytes``,
    when given (SK-02's absolute floor), FAILs any live task under that many
    bytes free regardless of what the baseline says or the tolerance -- the
    httpd stack blob class (project memory project_httpd_stack_blob_class) is
    exactly a task that looked fine relative to its own history but was
    dangerously close in absolute terms.

    ``board_fw_commit``/``baseline_fw_commits``, when both given, let any
    drop be scored against whether the comparison is even apples-to-apples:

      * same, known commit on both sides: within tolerance is PASS
        (ordinary run-to-run noise), beyond tolerance is FAIL (a real
        regression against a build that hasn't changed).
      * commit mismatch, or an unverified/missing/placeholder commit
        (None, "", "?", "unknown") on either side: INCONCLUSIVE regardless
        of how large the drop is. Two different firmware builds legitimately
        differ in stack usage, so a beyond-tolerance drop across a commit
        change is not evidence of a regression at all -- it can only ever be
        INCONCLUSIVE, never FAIL, unless the absolute floor below is also
        breached. The reason text still names every task whose drop exceeded
        tolerance and both commits, so the numbers stay visible even though
        the verdict is softened.

    ``min_free_bytes`` (SK-02's absolute floor) is the one check this commit
    logic never touches: it FAILs any live task under that many bytes free
    regardless of what the baseline says, the tolerance, OR any commit
    mismatch -- a task with almost no headroom is dangerous whatever build
    produced it (the httpd stack blob class, project memory
    project_httpd_stack_blob_class, is exactly a task that looked fine
    relative to its own history but was dangerously close in absolute
    terms).

    ``task_tolerance_overrides`` (default
    :data:`_STACK_MARGIN_TASK_TOLERANCE_OVERRIDES`) maps a task name to its
    own tolerance, which can only widen ``tolerance_bytes`` (the larger wins); pass ``{}`` to disable.

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

    if task_tolerance_overrides is None:
        task_tolerance_overrides = _STACK_MARGIN_TASK_TOLERANCE_OVERRIDES

    regressed_beyond_tolerance = []
    regressed_within_tolerance = []
    inconclusive_tasks = []
    for e in entries:
        base = baseline_by_name.get(e.name)
        if base is None:
            inconclusive_tasks.append(e.name)
            continue
        if not base.alive:
            continue
        drop = base.hwm_bytes - e.hwm_bytes
        if drop <= 0:
            continue
        task_tol = max(tolerance_bytes, task_tolerance_overrides.get(e.name, tolerance_bytes))
        entry_info = {"task": e.name, "hwm_bytes": e.hwm_bytes, "baseline_hwm_bytes": base.hwm_bytes,
                      "drop_bytes": drop, "tolerance_bytes": task_tol}
        if drop > task_tol:
            regressed_beyond_tolerance.append(entry_info)
        else:
            regressed_within_tolerance.append(entry_info)

    baseline_commits_set = set(baseline_fw_commits) if baseline_fw_commits else set()
    # A missing/placeholder commit on EITHER side is "unverified", never
    # "same": without a known pair there is no basis for calling a
    # within-tolerance drop noise on an identical build.
    board_commit_known = bool(board_fw_commit) and board_fw_commit not in _UNKNOWN_FW_COMMITS
    known_baseline_commits = {c for c in baseline_commits_set if c and c not in _UNKNOWN_FW_COMMITS}
    commit_unverified = not board_commit_known or not known_baseline_commits
    commit_mismatch = (not commit_unverified) and board_fw_commit not in known_baseline_commits

    observed = {
        "entries": [{"name": e.name, "hwm_bytes": e.hwm_bytes, "configured_stack_bytes": e.configured_stack_bytes} for e in entries],
        "below_floor": below_floor,
        "regressed_beyond_tolerance": regressed_beyond_tolerance,
        "regressed_within_tolerance": regressed_within_tolerance,
        "no_baseline": inconclusive_tasks,
        "tolerance_bytes": tolerance_bytes,
        "task_tolerance_overrides": dict(task_tolerance_overrides),
        "board_fw_commit": board_fw_commit,
        "baseline_fw_commits": sorted(baseline_commits_set),
        "commit_mismatch": commit_mismatch,
        "commit_unverified": commit_unverified,
    }

    if below_floor:
        # The absolute floor FAILs regardless of commit: a task with almost
        # no headroom is dangerous whatever the baseline says, and a commit
        # mismatch never softens this the way it does the tolerance checks
        # below.
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(below_floor)} task(s) below the {min_free_bytes} B absolute floor",
            observed=observed,
        )
    # A cross-commit (or unverified-commit) comparison can only ever be
    # INCONCLUSIVE: two different firmware builds legitimately differ in
    # stack usage, so a drop beyond tolerance there is not evidence of a
    # regression -- only evidence that the comparison isn't apples-to-apples.
    # Every task whose drop exceeded tolerance is still named in the reason,
    # alongside both commits, so the numbers stay visible even though the
    # verdict is softened from FAIL.
    if regressed_beyond_tolerance and (commit_mismatch or commit_unverified):
        names = ", ".join(f"{r['task']} (-{r['drop_bytes']} B > {r['tolerance_bytes']} B)" for r in regressed_beyond_tolerance)
        commit_desc = (
            f"fw_commit mismatch (board={board_fw_commit!r}, baseline={sorted(baseline_commits_set)!r})"
            if commit_mismatch
            else f"fw_commit unknown (board={board_fw_commit!r}, baseline={sorted(baseline_commits_set)!r})"
        )
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(
                f"{commit_desc}: {len(regressed_beyond_tolerance)} task(s) dropped beyond their "
                f"per-task noise tolerance below their committed baseline, but a cross-build "
                f"comparison cannot be scored as a regression: {names}"
            ),
            observed=observed,
        )
    # Same, known commit: a beyond-tolerance drop IS a real regression.
    if regressed_beyond_tolerance:
        names = ", ".join(f"{r['task']} (-{r['drop_bytes']} B > {r['tolerance_bytes']} B)" for r in regressed_beyond_tolerance)
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(regressed_beyond_tolerance)} task(s) dropped beyond their per-task noise "
                   f"tolerance below their committed baseline: {names}",
            observed=observed,
        )
    if regressed_within_tolerance and commit_mismatch:
        names = ", ".join(r["task"] for r in regressed_within_tolerance)
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(
                f"fw_commit mismatch (board={board_fw_commit!r}, baseline={sorted(baseline_commits_set)!r}): "
                f"{len(regressed_within_tolerance)} task(s) within the {tolerance_bytes} B noise tolerance "
                f"cannot be scored against a different build: {names}"
            ),
            observed=observed,
        )
    if regressed_within_tolerance and commit_unverified:
        names = ", ".join(r["task"] for r in regressed_within_tolerance)
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(
                f"fw_commit unknown (board={board_fw_commit!r}, baseline={sorted(baseline_commits_set)!r}): "
                f"{len(regressed_within_tolerance)} task(s) within the {tolerance_bytes} B noise tolerance "
                f"cannot be confirmed as same-build noise: {names}"
            ),
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
                           unacknowledged_crash: bool, floor_bytes: int = 8704) -> CaseResult:
    """SK-04: internal DRAM largest free block never below the floor; no
    UNACKNOWLEDGED CRASH REPORT banner.

    ``floor_bytes`` re-derived 2026-09-23, then corrected in review the same
    day: the original 11900 (Wave 0) was never actually a largest-free-block
    figure at all -- it was firmware's own
    ``KILN_DRAM_FREE_ALARM_BYTES`` (11903, a *total-free* alarm,
    ``firmware/KilnFW/App/drivers/common/dram_margin.h``), misapplied here to
    a *largest-contiguous-block* reading. The correct anchor is firmware's
    own largest-block alarm, ``KILN_DRAM_LARGEST_ALARM_BYTES = 8704``
    (``dram_margin.h``, the largest-free-block value at which a real failure
    happened 2026-08-22) -- a first re-derivation attempt set this to 8192
    (a 2026-09-21 measured reading, ROADMAP.md) without checking it against
    that alarm, which would have let a board firmware itself calls
    largest-block-low pass this case. Measured ``largest_free_block``
    history: ~7936-8192 B on 2026-09-21 (below today's floor -- expected,
    that was the regressed reading the alarm exists for), 9216 B on the
    current bench board (a06389f9, 2026-09-24, post-DRAM-fixes baseline --
    still comfortably above 8704). ``min_free`` (32787/14015 B in the same
    read) is a separate, already-healthy number this floor does not gate --
    it only ever concerns the single largest contiguous free block, the
    number that actually predicts whether one more large allocation can
    succeed."""
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
                                   trip_reason: Optional[int], trip_mask: Optional[int] = None,
                                   boot_loop_corroborated: Optional[bool] = None) -> CaseResult:
    """SP-02: link up, state armed/idle as appropriate, boot reason not
    watchdog, trip_reason 0, and (when both fields were actually parsed)
    trip_mask consistent with trip_reason via the same formula
    (`link_frame_trip_mask_for_reason()`).

    `trip_reason=None` means the report did not carry the field at all --
    this must NOT be treated as "0 / no trip" (that silently passes a
    board that IS tripped but whose report couldn't be parsed) -- it is
    INCONCLUSIVE instead, distinct from both PASS and the FAIL a real
    nonzero trip_reason produces.

    A lone `boot_reason == "watchdog"` is downgraded to INCONCLUSIVE, not
    FAIL -- see judge_pico_slot_metadata's docstring: OpenOCD's rp2040 SWD
    reset path reboots via the watchdog too, so this alone is indistinguishable
    from an ordinary debug_program(peer="pico") reset that just happened.
    `boot_loop_corroborated` (boot_id churn between two reads in the same
    run, or a DEAD required task) escalates it back to FAIL when supplied."""
    if not link_up:
        return CaseResult(Verdict.FAIL, reason="link is not up", observed={"link_up": link_up})
    if boot_reason == "watchdog":
        if boot_loop_corroborated:
            return CaseResult(
                Verdict.FAIL,
                reason="boot reason is watchdog, corroborated by a second boot-loop signal",
                observed={"boot_reason": boot_reason, "boot_loop_corroborated": boot_loop_corroborated},
            )
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(
                "boot reason is watchdog, but OpenOCD's SWD reset path also reboots the "
                "rp2040 via its own watchdog (docs/audits/short_proof_run_completed_"
                "2026-09-09.md:33-36) -- indistinguishable from a real boot loop "
                "without corroboration, which was not supplied"
            ),
            observed={"boot_reason": boot_reason},
        )
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
    answering without a session, a listed page shell (route_tier_table.h's
    kPageShellUris[]) answering 200 without one either way, other ADMIN
    routes answering 401/403/3xx without one when the board currently has
    web auth enabled, and
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

#: Appended to a page-mismatch FAIL reason (the case waited for a specific
#: page after a click_by_name() that itself replied "ok") so a screen-idle
#: swallow is named as a likely cause rather than a reader having to already
#: know about it: screen_idle.c/lvgl_port.c's touch_swallow path wakes the
#: panel on a tap but swallows that same tap, so click_by_name() still
#: replies "ok" while nothing actually navigated -- or, on firmware that
#: reports a swallow directly (KILN_UI_CLICK_SWALLOWED, and
#: KILN_UI_CLICK_VERDICT_UNKNOWN when the verdict wait timed out), this hint
#: instead covers the case where the verdict-wait itself timed out
#: unresolved.
#: cases_lcd.py's `_wake_and_home()` runs before each case that clicks a
#: named target, so this should be rare after that fix, but a case can still
#: race a NEW idle-blank between two of its own clicks (e.g. LCD-09/14/16's
#: multi-hop navigation) if one of its steps takes longer than the display
#: timeout.
BLANKED_SCREEN_HINT = (
    " (if spurious: the panel may have auto-blanked and swallowed this tap "
    "-- screen_idle.c/lvgl_port.c's touch_swallow path wakes the screen on "
    "a tap but swallows that same tap; on older firmware click_by_name() "
    "still replies 'ok' while nothing actually navigated, and on firmware "
    "carrying KILN_UI_CLICK_VERDICT_UNKNOWN the swallow can instead surface "
    "as click_by_name()'s own bounded verdict-wait timing out unresolved)"
)


def _find_target(targets: "list[dict]", name: str) -> "Optional[dict]":
    """Case-insensitive lookup: firmware labels are Title Case (e.g.
    ui_page_home.c's "Start") while callers here pass lowercase literals
    ("start"). Comparing case-insensitively avoids a spurious miss on that
    mismatch alone; call sites keep whatever literal spelling reads best."""
    name_lower = name.lower()
    for t in targets:
        target_name = t.get("name")
        if target_name is not None and target_name.lower() == name_lower:
            return t
    return None


def judge_lcd_home_idle(page: str, targets: "list[dict]",
                         start_matches_accent4: Optional[bool],
                         pause_is_hidden: Optional[bool],
                         color_debug: "Optional[dict]" = None) -> CaseResult:
    """LCD-01: home page, idle. Start button present and tappable, Pause
    hidden, and (when a capture was available) Start reads the ACCENT_4
    color. A missing camera capture degrades the color half to
    INCONCLUSIVE rather than failing the whole case on a busy webcam
    (CLAUDE.md: ffmpeg exit -5 means busy, never treated as a board defect).

    ``color_debug`` (from cases_lcd._try_capture_and_sample) carries the raw
    sampled/bezel RGB, region coords, distances and thresholds behind
    ``start_matches_accent4``/``pause_is_hidden`` -- attached to
    ``observed`` and, when a frame was actually saved, its path to
    ``evidence`` -- so a verdict here can be corroborated after the fact
    instead of resting on a bare bool with nothing to check it against
    (the 2026-09-24 LCD-01 FAIL had no sampled RGB, no bezel reference, and
    an empty captures/ dir alongside it).
    """
    observed = {"page": page, "targets": targets}
    evidence = []
    if color_debug:
        observed["color_debug"] = color_debug
        capture_path = color_debug.get("capture_path")
        if capture_path:
            evidence.append(capture_path)
    if page != "home":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'home'" + BLANKED_SCREEN_HINT, observed=observed, evidence=evidence)
    start = _find_target(targets, "start")
    if start is None or start.get("hidden"):
        return CaseResult(Verdict.FAIL, reason="Start target missing or hidden on home/idle", observed=observed, evidence=evidence)
    pause = _find_target(targets, "pause")
    if pause is not None and not pause.get("hidden") and pause_is_hidden is not False:
        # pause_is_hidden (from a camera sample) can override a firmware
        # 'hidden' flag disagreement; absent camera data, trust the flag.
        return CaseResult(Verdict.FAIL, reason="Pause target is not hidden on home/idle", observed=observed, evidence=evidence)
    if start_matches_accent4 is False:
        # A Start-button color mismatch always FAILs. The background
        # reference sample (cases_lcd._try_capture_and_sample's
        # "bg_reference") is diagnostic only: it may annotate the reason,
        # never soften the verdict. Review of the 2026-09-24 capture: the
        # reference point (widget 5,5) maps to a few frame pixels inside the
        # panel edge, and on that capture it landed on the bezel --
        # RGB(6,13,22) against a bezel of RGB(6,11,16), chroma offset 0.156,
        # just over the 0.15 threshold. Letting it downgrade the verdict
        # meant a few pixels of geometry drift would turn a real wrong-color
        # Start button into INCONCLUSIVE. The same capture shows the Start
        # button plainly green; the Start sample itself (frame 902,579) sat
        # on the button's top edge, which points at stale FRAME_CORNERS (a
        # geometry problem), not a color cast.
        reason = "Start button region does not read as ACCENT_4"
        bg_ref = (color_debug or {}).get("bg_reference")
        if bg_ref and bg_ref.get("reads_as_bezel"):
            reason += (
                " (background reference point reads as bezel: the widget-to-frame "
                "geometry is likely off, so the Start sample may not be on the button either)"
            )
        elif bg_ref and bg_ref.get("cast_suspected"):
            # Same downgrade as bg_out_of_tolerance below, for the stronger
            # cast signal -- this branch used to only annotate `reason` and
            # fall through to the hard FAIL at the end of this function, so a
            # background reference that tripped the (lower) cast_suspected
            # chroma bar still produced FAIL instead of INCONCLUSIVE, even
            # though it is stronger camera-cast evidence than the
            # bg_out_of_tolerance branch that DOES downgrade. Evidence: run
            # 20260925T055234Z's bg_reference read chroma offset 0.1521
            # (just over CAST_CHROMA_THRESHOLD 0.15, RGB (61,171,215) against
            # target RGB(26,31,43)) yet the case reported FAIL on a frame
            # with camera cast at least as bad as the case that does
            # downgrade.
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=(
                    reason
                    + f" (background reference also reads chroma-offset {bg_ref.get('chroma_offset')}, "
                    f"threshold {bg_ref.get('cast_threshold')}: check camera geometry and color cast "
                    "before treating this as a firmware color defect)"
                ),
                observed=observed,
                evidence=evidence,
            )
        elif bg_ref and bg_ref.get("bg_out_of_tolerance"):
            # The background reference itself is wrong by absolute distance
            # and/or chroma (below the stronger cast_suspected bar above, but
            # still outside the same tolerances a button color is judged
            # against) -- 2026-09-25 evidence: bg sampled RGB(52,90,111)
            # against target RGB(26,31,43), distance 93.7 (> COLOR_MATCH_
            # TOLERANCE 45) while chroma offset 0.0717 stayed under
            # CAST_CHROMA_THRESHOLD. A wrong background this way means camera
            # exposure/cast is suspect, so a button mismatch on the SAME
            # frame is not trustworthy evidence of a real firmware defect --
            # downgrade to INCONCLUSIVE rather than FAIL. This never softens
            # a mismatch when the background reads clean (see the sibling
            # test asserting a good background + wrong button still FAILs).
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=(
                    reason
                    + f" (background reference off by distance {bg_ref.get('distance')} "
                    f"[tolerance {bg_ref.get('distance_tolerance')}], chroma offset "
                    f"{bg_ref.get('chroma_offset')} [tolerance {bg_ref.get('chroma_tolerance')}]: "
                    "camera exposure/cast suspect, not treated as a firmware color defect)"
                ),
                observed=observed,
                evidence=evidence,
            )
        return CaseResult(Verdict.FAIL, reason=reason, observed=observed, evidence=evidence)
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
            evidence=evidence,
        )
    start_dbg = (color_debug or {}).get("start") or {}
    if start_dbg.get("matched_via_cast_fallback"):
        # Still a PASS (the widget checks all held and the surviving two
        # channels match), but say plainly that the color half is weaker
        # evidence than usual -- see cases_lcd._try_capture_and_sample().
        channel = "RGB"[start_dbg["cast_channel"]] if start_dbg.get("cast_channel") in (0, 1, 2) else "?"
        return CaseResult(
            Verdict.PASS,
            reason=(
                f"Start color matched only via the degraded cast fallback: the background reference "
                f"read the {channel} channel clipped, so {channel} was dropped and only the other two "
                f"channels' ratio was compared (degraded chroma distance "
                f"{start_dbg.get('degraded_chroma_distance')}, tolerance {start_dbg.get('chroma_tolerance')}). "
                f"This cannot tell ACCENT_4 from a hue differing mainly in {channel} (e.g. ACCENT_1 orange), "
                "nor a camera white-balance cast from a panel that lost that channel -- fix the camera "
                "white balance and re-run for a full-color check"
            ),
            observed=observed,
            evidence=evidence,
        )
    return CaseResult(Verdict.PASS, observed=observed, evidence=evidence)


def judge_lcd_config_hub(page: str, targets: "list[dict]",
                          expected_tiles: "tuple[str, ...]" = (
                              "Profiles", "Temperature", "Network / Wi-Fi", "Diagnostics",
                          )) -> CaseResult:
    """LCD-08: config hub reached by tapping the topbar gear ("settings").
    `expected_tiles` must match the firmware's exact button-label text
    (`build_nav_item()` calls in ui_page_config.c, e.g. "Network / Wi-Fi",
    not a shortened "Network" -- `kiln_ui_click_by_name()`
    (firmware/KilnFW/App/drivers/ui/kiln_ui.c) matches tap names with an
    exact `strcmp()`, so a literal that drifts from the button's real label
    both fails this presence check AND could never be clicked by name
    either. `expected_tiles` omits
    Touch Calibration by default since the plan marks it 'if present' --
    callers that know the board has it should pass the 5-tuple."""
    observed = {"page": page, "targets": targets}
    if page != "config":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'config'" + BLANKED_SCREEN_HINT, observed=observed)
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


#: LCD-22 tolerance when comparing a segment value read back over HTTP
#: (JSON floats) against the value the case computed.
LCD_EDIT_FIRING_TOL = 0.01


def judge_lcd_edit_firing(orig: dict, expected: dict, status: "dict | None",
                          content: "dict | None", exec_before: "dict | None",
                          exec_after: "dict | None", edit_targets: "dict | None",
                          refusal_before: "str | None" = None) -> CaseResult:
    """LCD-22: after tapping a future segment's target "+" and dwell "+" on the
    Edit firing page and Apply, the live working copy and the executor must
    reflect both edits and nothing else. Pure data in, CaseResult out.

    `orig`/`expected` carry seg0_target_c, seg0_dwell_min, seg1_target_c,
    seg1_dwell_min (what the profile started with / what it must read now).
    `status`/`content` are profile_live get_live_status()/get_live_content()
    dicts, `exec_*` are {state_name, profile_id, segment_count}, and
    `edit_targets` is the edit_firing page's list_tap_targets() dict. A None
    status/content (status read failed) is INCONCLUSIVE; a read that succeeded and
    shows the change absent is FAIL ("change not picked up")."""
    if status is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="could not read the live profile status after Apply",
                          observed={"status": status, "content": content})
    # status readable but no content: if status shows a working copy the
    # content read failed (unreadable board) -> INCONCLUSIVE; if it shows none
    # (409: no working copy) that is the "Apply did nothing" shape -- a FAIL
    # below.
    if content is None:
        try:
            has_copy = bool(status.get("active")) and int(status.get("working_id", -1)) >= 0
        except (TypeError, ValueError):
            has_copy = False
        if has_copy:
            return CaseResult(Verdict.INCONCLUSIVE, observed={"status": status, "content": None},
                              reason="the live status shows a working copy but its content could not be read")
    content = content or {}
    problems = []
    observed: dict = {"status": status, "content": content, "exec_before": exec_before,
                      "exec_after": exec_after, "expected": expected}
    if not status.get("active") or int(status.get("working_id", -1)) < 0:
        problems.append("change not picked up: no live working copy exists after Apply")
    # A last_refusal identical to the pre-run snapshot predates this run.
    if status.get("last_refusal") and status.get("last_refusal") != refusal_before:
        problems.append(f"executor refused the edit (last_refusal={status.get('last_refusal')!r})")
    segs = content.get("segments") or []
    if len(segs) < 2:
        problems.append(f"working copy has {len(segs)} segment(s), expected at least 2")
    else:
        def _eq(seg, key, want):
            try:
                return abs(float(seg.get(key)) - float(want)) <= LCD_EDIT_FIRING_TOL
            except (TypeError, ValueError):
                return False
        if not _eq(segs[1], "target_c", expected["seg1_target_c"]):
            problems.append(
                f"change not picked up: segment 2 target is {segs[1].get('target_c')!r}, "
                f"expected {expected['seg1_target_c']}")
        if not _eq(segs[1], "dwell_min", expected["seg1_dwell_min"]):
            problems.append(
                f"change not picked up: segment 2 dwell is {segs[1].get('dwell_min')!r}, "
                f"expected {expected['seg1_dwell_min']}")
        if not _eq(segs[0], "target_c", orig["seg0_target_c"]) or not _eq(segs[0], "dwell_min", orig["seg0_dwell_min"]):
            problems.append("segment 1 (the running one) changed although only segment 2 was edited")
    if not exec_after or exec_after.get("state_name") != "running":
        problems.append(f"executor is not running after Apply ({(exec_after or {}).get('state_name')!r})")
    elif exec_before and exec_after.get("profile_id") != exec_before.get("profile_id"):
        problems.append("executor profile_id changed across Apply")
    scroll = judge_lcd_no_scroll_budget({"edit_firing": edit_targets}) if edit_targets else None
    if scroll is not None and scroll.verdict == Verdict.FAIL:
        problems.append(scroll.reason)
    observed["no_scroll"] = scroll.observed if scroll is not None else None
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


# ---------------------------------------------------------------------------
def _lcd_edit_segs_problems(content, want: "list[dict]", label: str) -> "list[str]":
    """Compare the live working copy's segments with `want` (a list of
    {target_c, ramp_c_per_hr, dwell_min}) within LCD_EDIT_FIRING_TOL."""
    segs = (content or {}).get("segments") or []
    if len(segs) != len(want):
        return [f"{label}: working copy has {len(segs)} segment(s), expected {len(want)}"]
    out = []
    for i, (got, exp) in enumerate(zip(segs, want)):
        for key in ("target_c", "ramp_c_per_hr", "dwell_min"):
            try:
                ok = abs(float(got.get(key)) - float(exp[key])) <= LCD_EDIT_FIRING_TOL
            except (TypeError, ValueError):
                ok = False
            if not ok:
                out.append(f"{label}: segment {i + 1} {key} is {got.get(key)!r}, expected {exp[key]}")
    return out


def _lcd_edit_wid(status) -> int:
    try:
        return int((status or {}).get("working_id", -1))
    except (TypeError, ValueError):
        return -1


def judge_lcd_edit_ramp_steppers(expected: "list[dict]", status: "dict | None", content: "dict | None",
                                 exec_after: "dict | None", refusal_before: "str | None" = None) -> CaseResult:
    """LCD-23 part 1: after Next + the minus/plus steppers on a FUTURE segment
    (target -, ramp +, ramp +, ramp -, dwell -) and Apply, the live working
    copy must hold exactly `expected` (every field of every segment: the ramp
    net +5 C/hr, target -5 C, dwell -5 min on segment 2, segment 1 untouched),
    with no executor refusal and the firing still running."""
    if status is None or content is None:
        return CaseResult(Verdict.INCONCLUSIVE, observed={"status": status, "content": content},
                          reason="could not read the live profile status/content after Apply")
    problems = []
    if not status.get("active") or _lcd_edit_wid(status) < 0:
        problems.append("change not picked up: no live working copy exists after Apply")
    problems += _lcd_edit_segs_problems(content, expected, "after Apply")
    if status.get("last_refusal") and status.get("last_refusal") != refusal_before:
        problems.append(f"executor refused the edit (last_refusal={status.get('last_refusal')!r})")
    if not exec_after or exec_after.get("state_name") != "running":
        problems.append(f"executor is not running after Apply ({(exec_after or {}).get('state_name')!r})")
    observed = {"status": status, "content": content, "exec_after": exec_after, "expected": expected}
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_edit_refusal(adopted: "list[dict]", wid_before: int, status_after: "dict | None",
                           content_after: "dict | None", exec_after: "dict | None",
                           refusal_before: "str | None", locked_seen: bool,
                           http_window: "tuple | None", http_bound: "tuple | None",
                           content_after_http: "dict | None",
                           http_ceiling: "tuple | None" = None) -> CaseResult:
    """LCD-23 part 2: an edit the firmware must refuse. The finished segment's
    steppers must vanish from the Edit page (locked_seen), the LCD Apply of a
    stale edit to that segment must adopt NOTHING (working copy still equals
    `adopted`, same working_id, no executor refusal record -- the LCD's own
    window check turned it away before the executor saw it), the HTTP window
    violation must answer 409 and the HTTP bound violation 400 (each a
    (status, detail) tuple, status None when the call was accepted), and the
    working copy must still equal `adopted` afterwards. `http_ceiling` (None
    = probe not run) is the same (status, detail) for a target 10 C above the
    zone's max_temp_c, which only the HARD validator refuses: it must be 400."""
    if status_after is None or content_after is None or content_after_http is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="could not read the live profile after the refused edits",
                          observed={"status_after": status_after, "content_after": content_after})
    problems = []
    if not locked_seen:
        problems.append("the finished segment's steppers were still tappable on the Edit page (not locked)")
    problems += _lcd_edit_segs_problems(content_after, adopted, "after the LCD Apply of a finished-segment edit")
    if _lcd_edit_wid(status_after) != wid_before:
        problems.append(f"working_id changed across the refused edit ({wid_before} -> {_lcd_edit_wid(status_after)})")
    if status_after.get("last_refusal") != refusal_before:
        problems.append(f"an edit reached the executor and was refused there "
                        f"(last_refusal={status_after.get('last_refusal')!r}); the LCD should refuse it first")
    if not exec_after or exec_after.get("state_name") != "running":
        problems.append(f"executor is not running after the refused edits ({(exec_after or {}).get('state_name')!r})")
    checks = [("window violation", http_window, 409), ("bound violation", http_bound, 400)]
    if http_ceiling is not None:
        checks.append(("zone-ceiling violation", http_ceiling, 400))
    for label, got, want in checks:
        status = got[0] if got else None
        if status != want:
            problems.append(f"HTTP {label} answered {status!r}, expected {want} ({got[1] if got else 'no result'})")
    problems += _lcd_edit_segs_problems(content_after_http, adopted, "after the HTTP refusals")
    observed = {"status_after": status_after, "content_after": content_after, "exec_after": exec_after,
                "locked_seen": locked_seen, "http_window": http_window, "http_bound": http_bound,
                "http_ceiling": http_ceiling}
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_edit_firing_end(adopted: "list[dict]", own_wid: int, end_state: "str | None",
                              lcd_ended: "bool | None", status_ended: "dict | None",
                              content_ended: "dict | None", discard_error: "str | None",
                              status_discarded: "dict | None") -> CaseResult:
    """LCD-24: a firing with an adopted live edit ends on its own (state done).
    The Edit page, left open, must show the ended state (Apply and steppers
    gone -- `lcd_ended`); the live status must then report pending_decision
    with the same working copy; the LCD has NO decision UI (owner decision:
    "leave the working copy's end-of-run decision to the web"), so the
    decision is made over HTTP: Discard must clear the working copy."""
    if end_state is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="the firing did not end within the bounded wait",
                          observed={})
    if status_ended is None or content_ended is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="could not read the live profile after the firing ended",
                          observed={"end_state": end_state})
    problems = []
    if end_state != "done":
        problems.append(f"firing ended as {end_state!r}, expected 'done'")
    if lcd_ended is not True:
        problems.append("the Edit page still offered Apply/steppers after the firing ended"
                        if lcd_ended is False else "could not confirm the Edit page's ended state")
    if not status_ended.get("pending_decision"):
        problems.append("live status shows no pending_decision after the firing with an edit ended")
    if _lcd_edit_wid(status_ended) != own_wid:
        problems.append(f"working_id after the end is {_lcd_edit_wid(status_ended)}, expected {own_wid}")
    problems += _lcd_edit_segs_problems(content_ended, adopted, "after the firing ended")
    if discard_error:
        problems.append(f"decide discard failed: {discard_error}")
    elif status_discarded is None:
        problems.append("could not read live status after discard")
    else:
        if _lcd_edit_wid(status_discarded) >= 0:
            problems.append("a working copy remains after decide discard")
        if status_discarded.get("pending_decision"):
            problems.append("pending_decision still true after decide discard")
    observed = {"end_state": end_state, "lcd_ended": lcd_ended, "status_ended": status_ended,
                "content_ended": content_ended, "status_discarded": status_discarded}
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


# LCD-02/03/04/09/14/16/19 (Wave 2, plan doc §3.8/§8). Same split as the
# Wave 1c block above: these take already-fetched plain data and return a
# CaseResult, with no I/O of their own -- cases_lcd.py does the fetching
# (UI_TEST navigation, thermo/profile/safety reads, best-effort webcam
# sampling via lcd_sampler.py).
# ---------------------------------------------------------------------------

#: ui_page_diagnostics.c's UI_PAGE_DIAGNOSTICS_PAGE_COUNT (8 sub-pages,
#: index 0 Firmware .. index 7 Crash Report -- the round-3 rewrite of
#: LCD-16 no longer looks these up by a title-click that was never a real
#: button (see judge_lcd_diagnostics_pages()'s docstring for why).
DIAGNOSTICS_PAGE_COUNT = 8


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


def judge_lcd_profiles_picker(page: str, rows: "list[dict]", paging_present: Optional[bool],
                               new_icon_present: Optional[bool], detail_page: Optional[str],
                               max_rows: int = 4,
                               profiles_count: Optional[int] = None) -> CaseResult:
    """LCD-09: profiles picker reached by tapping Profiles from the hub.
    `rows` is the list-area tap targets located by POSITION
    (cases_lcd.py's `_profile_rows_by_position()`) -- `build_row()`
    (ui_page_profile_picker.c) tags no fixed name on a row's own button, so
    there is no `profile_row_*` name to match on real firmware.

    `profiles_count`, when known (ProfilesClient.list_all() over the wire),
    is a cross-check independent of anything sampled from the screen: if
    the board itself reports at least one profile but the list area shows
    no rows at all, that is a real defect (a stuck/empty list), not merely
    undecidable -- so this turns the previously-blanket INCONCLUSIVE into a
    FAIL specifically when a positive count is known. `profiles_count is
    None` (the count could not be read) or `== 0` (genuinely no profiles)
    both still fall through to the original INCONCLUSIVE, since neither
    contradicts an empty list area."""
    observed = {
        "page": page, "row_count": len(rows), "paging_present": paging_present,
        "new_icon_present": new_icon_present, "detail_page": detail_page,
        "profiles_count": profiles_count,
    }
    if page != "profiles":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'profiles'" + BLANKED_SCREEN_HINT, observed=observed)
    if len(rows) > max_rows:
        return CaseResult(Verdict.FAIL, reason=f"{len(rows)} rows shown, expected <= {max_rows}", observed=observed)
    favorites = [r for r in rows if r.get("starred")]
    if favorites and any(not r.get("starred") for r in rows[: len(favorites)]):
        return CaseResult(Verdict.FAIL, reason="favorite row(s) are not sorted first", observed=observed)
    # paging_present / new_icon_present are Optional: cases_lcd.py's
    # _profiles_topbar_icons() finds the topbar's Prev/Next/New icons by
    # POSITION, since ui_topbar.c's build_icon() sets no tap-name tag and
    # kiln_ui.c's tap walk falls back to the button's label text -- the
    # LVGL symbol glyph, which arrives on the PC as U+FFFD bytes. The icons
    # ARE listed tap targets, so a missing one is a real FAIL; only a state
    # the bench genuinely cannot decide (None) is INCONCLUSIVE.
    if paging_present is False:
        return CaseResult(Verdict.FAIL, reason="Prev/Next paging icons missing from the topbar", observed=observed)
    if new_icon_present is False:
        return CaseResult(Verdict.FAIL, reason="New profile icon missing from the topbar", observed=observed)
    if rows and detail_page != "profile_detail":
        return CaseResult(
            Verdict.FAIL,
            reason=f"tapping a row opened {detail_page!r}, expected 'profile_detail'",
            observed=observed,
        )
    if new_icon_present is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="could not locate the topbar icons (no 'back'/'home' anchor target)",
            observed=observed,
        )
    if paging_present is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=("Prev/Next both disabled (a single page of profiles, or undecidable "
                    "from tap positions) -- paging could not be confirmed"),
            observed=observed,
        )
    if not rows:
        if profiles_count is not None and profiles_count > 0:
            return CaseResult(
                Verdict.FAIL,
                reason=(f"profiles_count={profiles_count} but the list area shows no rows"),
                observed=observed,
            )
        return CaseResult(Verdict.INCONCLUSIVE, reason="no profile rows present to confirm row-tap navigation", observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_temperature_page(page: str, zone_rows_rendered: "dict[int, Optional[bool]]",
                                readings: "dict[int, float]",
                                safety_line_rendered: Optional[bool],
                                expect_safety_on: bool,
                                configured_zones: Optional[int] = None,
                                header_rendered: Optional[bool] = None,
                                zero_zone_label_rendered: Optional[bool] = None) -> CaseResult:
    """LCD-14 (round 3 rewrite): the temperature page's zone-temp and Safety
    (K4) labels are plain, non-clickable `lv_label`s (`ui_page_temperature.c`
    never tap-tags them), so they never appear in `list_tap_targets()` at
    all -- the old `zone_temp_*`/`safety_line` name lookups this replaces
    could only ever return not-found. `thermo_read()` values are real, but
    they read the SAME underlying MAX31856 channels the LCD's own labels are
    fed from over the internal SPI bus, not the rendered pixels, so
    comparing one against the other can only prove "the firmware's own read
    path returns something", not "the panel is displaying it correctly" --
    that half is dropped rather than kept as a fake confirmation.

    What this can actually judge: whether `configured_zones` distinct
    zone-row regions render as non-background content by webcam capture
    (`zone_rows_rendered`, keyed by zone index, True/False/None per region --
    see docs/COMMISSIONING_LCD_RUNBOOK.md for the sampled row geometry), and
    the same presence check for the Safety (K4) line
    (`safety_line_rendered`) cross-checked only for SANITY against whether a
    firing is active (`expect_safety_on`) -- rendered-or-not, never a color
    or value match, since neither is derivable from a plain label over the
    wire. `readings` is carried in `observed` for a human reviewer's
    reference only and never gates the verdict, since it proves nothing
    about what the panel renders.

    `configured_zones` is the board's own authoritative zone count (over the
    UART CONTROL link's GET_ZONES, cases_lcd.py's
    `_configured_zone_count()`) -- the exact value
    `ui_page_temperature.c`'s `ui_page_temperature_build()` uses to decide
    how many zone rows to build, replacing an earlier `max(len(readings), 3)`
    guess an opus review of d66ba612 found could PASS a genuinely missing
    zone row: that guess could read 3 while the board renders only 2 real
    rows, in which case the "3rd" sample lands on the Relays card header
    (rendered one row higher than a real 3rd row would sit) and reads as
    legitimate content. `configured_zones is None` (the GET_ZONES query
    failed) is never treated as "assume 3" -- it goes straight to
    INCONCLUSIVE, since a fabricated zone count is exactly the failure mode
    being fixed. `configured_zones == 0` is a distinct, valid board state
    (`s_zone_count == 0`'s "No zones configured" label, a plain
    non-card-wrapped label at a different y than a real row -- see
    `zero_zone_label_rendered`/`_LCD14_ZERO_ZONE_LABEL_Y`), judged
    separately below rather than as "0 rows expected, 0 rows missing,
    trivially PASS".

    `header_rendered` (`configured_zones > 0` only) is the presence check at
    the y-position the Relays card's own header line is expected to land,
    computed from `configured_zones` via the SAME row-pitch formula real
    zone rows use (a card's top pad and a zone row's top pad are both
    UI_THEME_PADDING_PX/2, and each one's own first line sits the same
    offset below it -- see cases_lcd.py's module comment). This is the
    layout-shift check: if the board actually rendered ONE FEWER row than
    `configured_zones` -- a real missing-row defect -- the header shifts up
    by one full row pitch and this position reads as background instead.
    A per-row-only check cannot tell that shape apart from a genuine Nth
    row, since both read as non-background content at the position a
    correctly-rendered Nth row would occupy; `header_rendered is False`
    catches it directly. `header_rendered is None` with every zone row
    confirmed is INCONCLUSIVE, never PASS: without it the missing-row shape
    above is undecidable.

    Cannot detect: a zone row rendering a WRONG temperature value, a stale
    (frozen) value, or the Safety line rendering the wrong color/text while
    still being present -- only presence/absence of rendered content at
    each row's known position."""
    observed = {
        "page": page, "zone_rows_rendered": zone_rows_rendered, "readings": readings,
        "safety_line_rendered": safety_line_rendered, "expect_safety_on": expect_safety_on,
        "configured_zones": configured_zones, "header_rendered": header_rendered,
        "zero_zone_label_rendered": zero_zone_label_rendered,
    }
    if page != "temperature":
        return CaseResult(Verdict.FAIL, reason=f"page={page!r}, expected 'temperature'" + BLANKED_SCREEN_HINT, observed=observed)
    if configured_zones is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="the board's configured zone count could not be read (control_get_zones failed) "
                   "-- refusing to guess a zone count to check against",
            observed=observed,
        )
    if configured_zones == 0:
        if zero_zone_label_rendered is None:
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason="0 zones configured, but the 'No zones configured' label region was not "
                       "sampled (no camera capture)",
                observed=observed,
            )
        if zero_zone_label_rendered is False:
            return CaseResult(
                Verdict.FAIL,
                reason="0 zones configured but the 'No zones configured' label does not render",
                observed=observed,
            )
        return CaseResult(Verdict.PASS, observed=observed)
    if not zone_rows_rendered:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no zone-row regions were sampled (no camera capture)", observed=observed)
    missing = [z for z, rendered in zone_rows_rendered.items() if rendered is False]
    if missing:
        return CaseResult(Verdict.FAIL, reason=f"zone row(s) {missing} render as background (no content)", observed=observed)
    if len(zone_rows_rendered) < configured_zones or any(v is None for v in zone_rows_rendered.values()):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"only {sum(1 for v in zone_rows_rendered.values() if v)} of {configured_zones} zone rows were confirmed rendered",
            observed=observed,
        )
    if header_rendered is None:
        # The header sample is the only thing that tells "N rows" apart from
        # "N-1 rows, header shifted up" (see above) -- an undecided reading
        # must not let every-row-looks-fine fall through to PASS.
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="the Relays card header position was not sampled, so a shifted-up header "
                   "(one fewer zone row rendered than configured) cannot be ruled out",
            observed=observed,
        )
    if header_rendered is False:
        return CaseResult(
            Verdict.FAIL,
            reason=(f"the Relays card header does not render at its expected position for "
                    f"{configured_zones} configured zone(s) -- it may have shifted up one row, "
                    "meaning fewer zone rows actually rendered than are configured"),
            observed=observed,
        )
    # safety_line_rendered is frequently None -- its y-position is dynamic
    # (it sits inside the Relays card, after a wrapped row whose height
    # depends on how many relay buttons wrapped), unlike the zone rows'
    # fixed top-of-page stack, so it is sampled best-effort only and a miss
    # here is never held against the run: a real absence is only checked
    # when a definite reading came back.
    if safety_line_rendered is False and expect_safety_on:
        return CaseResult(
            Verdict.FAIL,
            reason="Safety (K4) line does not render while a firing is active",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_diagnostics_pages(pages_paged: int, expected_pages: int,
                                 next_disabled_at_end: Optional[bool],
                                 crash_report_step: Optional[int],
                                 relay_life_has_reset: Optional[bool]) -> CaseResult:
    """LCD-16 (round 3 rewrite): ui_page_diagnostics.c's 8 sub-pages have no
    buttons named after their titles -- they are reached only by the
    topbar's untagged Next/Prev glyph icons, and the sub-page title itself
    (built by update_title() as "Diagnostics: %s  %u of %u") is not
    readable over the wire at all (ui_test_client.py has no label-read
    command) -- only a webcam capture could confirm it, which this case
    does not take. The old title-click loop this replaces was therefore
    unconditionally broken (NOT_FOUND on its very first click) and the old
    heap-value cross-check read a plain, non-clickable label
    (build_stat_row()) that was never a real tap target either.

    What IS honestly observable over the wire: `cases_lcd.py` pages forward
    with a raw coordinate touch at the Next icon's own position (position,
    not name, since Prev and Next share one undecodable glyph name and
    click_by_name() answers AMBIGUOUS for it on a live board), counting how
    many of the `expected_pages` forward hops actually changed the
    tap-target set (`pages_paged`); whether Next reads disabled once the
    last page is reached (`next_disabled_at_end`); at which hop (if any) the
    Crash Report page's "Acknowledge" button was seen (`crash_report_step`,
    only ever present when the board actually has an unacknowledged crash --
    its absence on a healthy board is normal and never at fault); and
    whether the Relay Life page still shows a Reset button
    (`relay_life_has_reset` -- UI_PLAN.md section 6.4 removed it, so any
    True here is a regression)."""
    observed = {
        "pages_paged": pages_paged, "expected_pages": expected_pages,
        "next_disabled_at_end": next_disabled_at_end,
        "crash_report_step": crash_report_step,
        "relay_life_has_reset": relay_life_has_reset,
    }
    if relay_life_has_reset:
        return CaseResult(Verdict.FAIL, reason="Relay Life page still shows a Reset button (pre-rework only)", observed=observed)
    if crash_report_step is not None and crash_report_step != expected_pages:
        return CaseResult(
            Verdict.FAIL,
            reason=f"Crash Report's Acknowledge button seen at paging step {crash_report_step}, expected the last step ({expected_pages})",
            observed=observed,
        )
    if pages_paged < expected_pages:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"paging stopped after {pages_paged}/{expected_pages} forward hops -- Next may be broken or a tap was swallowed",
            observed=observed,
        )
    if next_disabled_at_end is False:
        return CaseResult(Verdict.FAIL, reason="Next is still enabled on the last diagnostics sub-page", observed=observed)
    if next_disabled_at_end is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="could not determine whether Next is disabled on the last diagnostics sub-page",
            observed=observed,
        )
    return CaseResult(Verdict.PASS, observed=observed)


def judge_lcd_pin_lock(keypad_raised: Optional[bool], wrong_pin_refused: Optional[bool],
                        right_pin_started: Optional[bool], stop_gated: Optional[bool]) -> CaseResult:
    """LCD-19: after `lcd_timeout_min`, Start raises the keypad; a wrong PIN
    is refused; the right PIN starts; Stop IS gated too. Owner decision
    2026-09-28 ("stop needs login. there is an estop button") reversed the
    old "Stop is never gated" rule: with the lock engaged, a Stop tap must
    raise the PIN keypad before Confirm Stop, since the hardware E-stop is
    the independent backstop. Checked even when the others are
    INCONCLUSIVE."""
    observed = {
        "keypad_raised": keypad_raised, "wrong_pin_refused": wrong_pin_refused,
        "right_pin_started": right_pin_started, "stop_gated": stop_gated,
    }
    if keypad_raised is False:
        return CaseResult(Verdict.FAIL, reason="Start tap after the LCD timeout did not raise the PIN keypad", observed=observed)
    if wrong_pin_refused is False:
        return CaseResult(Verdict.FAIL, reason="a wrong PIN was not refused", observed=observed)
    if right_pin_started is False:
        return CaseResult(Verdict.FAIL, reason="the correct PIN did not start the firing", observed=observed)
    if stop_gated is False:
        return CaseResult(
            Verdict.FAIL,
            reason="Stop opened Confirm Stop without the PIN keypad; owner decision 2026-09-28 requires Stop to need the PIN",
            observed=observed,
        )
    if None in (keypad_raised, wrong_pin_refused, right_pin_started, stop_gated):
        # Name which stage(s) read None rather than a generic "missing
        # UI_TEST API or camera" -- 2026-09-25 (LCD-19 bench evidence,
        # 20260925T170357Z_full/summary.json): the keypad was confirmed
        # raised, yet the case still ended up INCONCLUSIVE with no way to
        # tell from this reason alone which downstream stage produced the
        # unresolved None (a first-digit click race, since fixed in
        # ui_test_client.enter_pin() -- see its
        # _ENTER_PIN_RETRY_POLL_S comment -- rather than a
        # missing API or camera at all).
        stage_names = {
            "keypad_raised": keypad_raised,
            "wrong_pin_refused": wrong_pin_refused,
            "right_pin_started": right_pin_started,
            "stop_gated": stop_gated,
        }
        unresolved = [name for name, value in stage_names.items() if value is None]
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=(
                f"could not exercise: {', '.join(unresolved)} (missing UI_TEST API or camera, "
                "or a click/entry step did not complete)"
            ),
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
    is `None` this is INCONCLUSIVE, never a silent PASS.

    Edge tolerance (found on hardware 2026-09-24, run
    20260924T072516Z_heat): `state_name` comes from `get_exec_status()` (the
    in-process profiles client) and `energized` comes from a separate
    `dashboard_http_client.get_status()` HTTP round trip -- two different
    calls, sampled at two different instants, not one atomic read. At the
    very first sample of a run, `state` can already read `running` one poll
    tick before the relay's HTTP response catches up (still reading
    `False`); at the very last sample, `state` can already read `done` one
    tick before the relay reads back `False`. That one-tick lag exactly at
    the start/stop transition is a harness sampling artifact, not evidence
    the relay energized *before* `running` went true -- the two fields were
    never read together to begin with, so no ordering claim between them can
    be made from a single pair of separate polls. Only the FIRST and LAST
    reported sample get this tolerance, and only when at least 3 samples
    were reported (so an interior one exists); any disagreement at an
    interior sample, or anywhere in a 1- or 2-sample series, is still a real
    failure."""
    if not samples:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no safety_relay_energized samples collected", observed={})
    reported = [(s, e) for s, e in samples if e is not None]
    if not reported:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="safety_relay_energized was never reported (no host, or a stale build)",
            observed={"samples": samples},
        )
    # The edge tolerance only applies once there is at least one INTERIOR
    # sample left to judge strictly -- with 1 or 2 reported samples every
    # sample is an edge, and tolerating both would make any such series a
    # vacuous PASS (e.g. [("running", False), ("done", True)]: the relay was
    # never once seen energized while running).
    last_idx = len(reported) - 1
    edges = (0, last_idx) if len(reported) >= 3 else ()
    offenders = [
        (s, e) for idx, (s, e) in enumerate(reported)
        if (s == "running") != bool(e) and idx not in edges
    ]
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
    """HP-06: owner decision 2026-09-28 ("stop needs login. there is an
    estop button.") re-tiered POST /api/profile_exec/stop from
    ROUTE_TIER_SAFETY_REDUCE to ROUTE_TIER_USER -- an unauthenticated POST
    must now be REFUSED (401, no session) and the firing must still be
    RUNNING afterward. This inverts HP-06's pre-2026-09-28 expectation
    (200 + stopped); the physical E-stop is the backstop for an
    unauthenticated user now, not this HTTP route."""
    if http_status != 401:
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/profile_exec/stop (no session) returned {http_status!r}, expected 401",
            observed={"status": http_status},
        )
    if state_after != "running":
        return CaseResult(
            Verdict.FAIL,
            reason="firing is no longer RUNNING after the refused, unauthenticated stop attempt",
            observed={"state": state_after},
        )
    return CaseResult(Verdict.PASS, observed={"status": http_status, "state": state_after})


def judge_firing_history(entries: "list[dict]", expected_name_prefix: str = "BENCH_") -> CaseResult:
    """HP-08: a completed bench run appears with the right profile name, a
    start time and an outcome.

    Field names here match what `firing_history_get_handler()` actually
    emits (`dashboard_format_firing_history_json()`,
    `profile_firing_run_record_t`): `profile_name`, `run_started_unix_s`,
    `duration_s`, `zone_mask`, `zones`. There is no separate `outcome`/
    `state` field on a record -- only a run that reached a terminal state
    (done/stopped/faulted) is persisted into history at all, so the record
    existing with a `duration_s` is itself the outcome evidence; `start_time`/
    `started` are kept as fallbacks for a future/alternate schema, not
    because today's firmware emits either spelling."""
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
        if not any(k in entry for k in ("run_started_unix_s", "start_time", "started")):
            return CaseResult(Verdict.FAIL, reason="a matching history entry has no start-time field", observed={"entry": entry})
        if not any(k in entry for k in ("duration_s", "outcome", "state")):
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
                     page_shell_open: Optional[bool], api_route_gated: Optional[bool],
                     login_ok: bool, session_ok: bool,
                     extend_ok: bool, restore_ok: bool, restore_matches: bool,
                     state: Optional[dict] = None) -> CaseResult:
    """WEB-SEC-03: enable web auth with the harness credential, verify the
    auth surface (dashboard still open, an ADMIN-tier page shell still
    answers 200 with no session, the data-bearing ADMIN route answers 401,
    login + session + extend all work), then disable and confirm the restore
    round-tripped.

    ``page_shell_open`` and ``api_route_gated`` are two separate probes,
    both required: firmware serves a listed page shell (e.g.
    ``/settings/zones``, route_tier_table.h's kPageShellUris[]) with 200 and
    no redirect to an unauthenticated GET (lazy login, owner decision
    2026-09-24: the web UI never shows a login until an action needs one),
    while ``GET /api/zones`` -- the data-bearing route -- still answers
    exactly 401. Neither probe is evidence for the other.

    Same "restore failure always FAILs, otherwise the verdict is about the
    write path" shape as :func:`judge_web_rw_toggle`, just with more
    write-path steps to check in order -- the first one that did not hold
    names the reason."""
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
    if not page_shell_open:
        return CaseResult(
            Verdict.FAIL,
            reason="/settings/zones (a page shell) did not answer 200 without a redirect with no session "
                   "while web auth was enabled",
            observed=observed,
        )
    if not api_route_gated:
        return CaseResult(
            Verdict.FAIL,
            reason="GET /api/zones did not answer 401 with no session while web auth was enabled",
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


def judge_web_sec04(pin_set_ok: bool, enabled_ok: bool, readback_enabled: Optional[bool],
                     restore_ok: bool, restore_matches: bool,
                     state: Optional[dict] = None) -> CaseResult:
    """WEB-SEC-04: ensure the admin LCD PIN from ``KILNCTL_LCD_PIN`` is
    configured (written only when ``admin_pin_set`` was false; otherwise
    ``pin_set_ok`` is passed True without a write), enable ``lcd_enabled``,
    confirm the readback shows it on, then restore all four policy fields
    to their original values. Unlike WEB-SEC-03's web password, an LCD PIN
    is one-way hashed with no read-back and no "clear just this PIN" route,
    so the restore here only ever concerns the policy fields, never the PIN
    hash itself.

    Same "restore failure always FAILs first" shape as
    :func:`judge_web_sec03`."""
    observed = dict(state or {})
    if not restore_ok or not restore_matches:
        # Name the first failing step too, when there was one, so a reader
        # doesn't have to guess whether the restore failure is the whole
        # story or the write path was already broken before it ran.
        prefix = ""
        if not pin_set_ok:
            prefix = "set_lcd_pin did not confirm ok:true for the harness admin PIN, and "
        elif not enabled_ok:
            prefix = "set_policy(lcd_enabled=1) did not report ok:true, and "
        elif not readback_enabled:
            prefix = "GET /api/auth/config did not show lcd_enabled:true after enabling it, and "
        return CaseResult(
            Verdict.FAIL,
            reason=prefix + "lcd_enabled policy restore did not round-trip -- board may be left with lcd_enabled changed",
            observed=observed,
        )
    if not pin_set_ok:
        return CaseResult(
            Verdict.FAIL, reason="set_lcd_pin did not confirm ok:true for the harness admin PIN", observed=observed
        )
    if not enabled_ok:
        return CaseResult(Verdict.FAIL, reason="set_policy(lcd_enabled=1) did not report ok:true", observed=observed)
    if not readback_enabled:
        return CaseResult(
            Verdict.FAIL,
            reason="GET /api/auth/config did not show lcd_enabled:true after enabling it",
            observed=observed,
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


# GET /api/zones printed pid_kp/ki/kd at %.4f before 2026-09-28 and at %.9g
# after, so a before/after read that straddles that firmware change (an OTA
# push across it, or a rollback past it) sees e.g. 0.6 vs 0.600000024 for an
# unchanged gain. Compare within %.4f's half-step (5e-5) plus a tiny relative
# term instead of exact dict equality; the hazard these checks exist for
# (firmware-default gains replacing tuned ones) is far larger than that.
_PID_GAIN_MATCH_ABS_TOL = 5e-5
_PID_GAIN_MATCH_REL_TOL = 1e-6


def pid_gains_match(before: "Optional[dict]", after: "Optional[dict]") -> bool:
    """True when two {zone_index: {pid_kp, pid_ki, pid_kd}} maps hold the same
    zones and every gain agrees within the GET-format tolerance above."""
    import math
    if before is None or after is None:
        return before is after
    if set(before) != set(after):
        return False
    for zi, gb in before.items():
        ga = after[zi]
        if not isinstance(gb, dict) or not isinstance(ga, dict) or set(gb) != set(ga):
            return False
        for key, vb in gb.items():
            va = ga[key]
            if not isinstance(vb, (int, float)) or not isinstance(va, (int, float)):
                if vb != va:
                    return False
                continue
            if not math.isclose(float(vb), float(va), rel_tol=_PID_GAIN_MATCH_REL_TOL,
                                abs_tol=_PID_GAIN_MATCH_ABS_TOL):
                return False
    return True


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
    if not pid_gains_match(pid_gains_before, pid_gains_after):
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


#: SafetyDiag.describe() (devices_safety.py) prints the field as
#: "boot reason: power-on | state armed | ..." -- a SPACE, a hyphenated
#: value, and "+"-joined bits when more than one reason bit is set (e.g.
#: "watchdog+brownout"). The first version of this parser looked for
#: "boot_reason" with an underscore and `\w+`, which matches NOTHING the
#: real device module ever emits and would have silently returned None on
#: hardware -- making every watchdog check below vacuous while the mocked
#: tests, which fed an invented "boot_reason: power_on", stayed green.
#: Both spellings are accepted now, and the whole "+"-joined run is
#: returned so callers test membership, never equality.
def parse_diag_boot_reason(diag_text: str) -> "str | None":
    """The boot-reason token SafetyDiag.describe() prints. None if the diag
    text carries no such field at all (never received / older firmware) --
    distinct from finding one that happens to name "watchdog"."""
    m = re.search(r"boot[_ ]reason\s*[:=]?\s*([\w+.-]+)", diag_text)
    return m.group(1) if m else None


#: ota_pico_relay.c's refusal text for SAFETY_LINK_UPDATE_STATE_REFUSED_
#: RUNNING_IMAGE_OVERLAP (state 9, SaftyFW update_task.c:943): the bench
#: Pico runs a FLAT image with no two-slot bootloader, so any relayed
#: write overlaps the running image and firmware refuses it by design.
#: That refusal is the correct, safe behaviour -- it is NOT an OTA defect,
#: so every OT-P judge grades it INCONCLUSIVE (the case's own precondition,
#: a bootloader-equipped Pico, was not met), never FAIL.
PICO_RUNNING_IMAGE_OVERLAP_SIGNATURE = "would overwrite its running flat image"


def pico_overlap_refusal(last_error: "Optional[str]") -> bool:
    return bool(last_error) and PICO_RUNNING_IMAGE_OVERLAP_SIGNATURE in str(last_error)


def judge_ota_pico_push_applied(
    phase: Optional[str],
    commit_after: "Optional[str]",
    expected_commit: "Optional[str]",
    boot_reason: "Optional[str]",
    commissioning_identical: Optional[bool],
    last_error: "Optional[str]" = None,
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
        "last_error": last_error,
    }
    if pico_overlap_refusal(last_error):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=("Pico refused the relay because it would overlap its running flat image "
                    "(no two-slot bootloader on this unit) -- the correct firmware behaviour, "
                    "not an OTA failure; this case needs a bootloader-equipped Pico"),
            observed=observed,
        )
    if phase != "done":
        return CaseResult(Verdict.FAIL, reason=f"phase={phase!r}, expected 'done'", observed=observed)
    if boot_reason and "watchdog" in boot_reason:
        return CaseResult(
            Verdict.FAIL,
            reason=f"Pico boot reason is {boot_reason!r} after the relay (2026-09-18 erase-time watchdog defect's signature)",
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
    last_error: "Optional[str]" = None,
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
        "last_error": last_error,
    }
    if pico_overlap_refusal(last_error):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=("the relay stopped at the running-image-overlap refusal, so the staged CRC "
                    "check this case exists to exercise was never reached"),
            observed=observed,
        )
    if not push_refused_or_failed:
        return CaseResult(Verdict.FAIL, reason="corrupt Pico image was accepted; expected a refusal/failed relay", observed=observed)
    if boot_reason and "watchdog" in boot_reason:
        return CaseResult(Verdict.FAIL, reason=f"Pico boot reason is {boot_reason!r} after a corrupt-image attempt", observed=observed)
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
    if pico_overlap_refusal(last_error):
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=("OT-P01's relay stopped at the running-image-overlap refusal -- no erase ever "
                    "ran, so this observer has nothing to judge"),
            observed=observed,
        )
    if boot_reason and "watchdog" in boot_reason:
        return CaseResult(Verdict.FAIL, reason=f"Pico boot reason is {boot_reason!r} (2026-09-18 erase-time watchdog defect)", observed=observed)
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
    if trip_pending is None:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="trip state unreadable: could not determine whether a trip was pending before push",
            observed=observed,
        )
    if trip_pending is False:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no trip was pending at push time -- the case's own precondition was not met",
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
    and every sampled temperature -- ONCE the zone has first arrived within
    ``hyst_c + margin_c`` of ``target_c`` -- stays within that same band.

    The run starts at ambient, well outside the band, and the initial
    ramp-up to `target_c` is expected to be outside it the whole way there
    -- that is not a cycling defect, and judging every sample from profile
    start (the pre-fix behaviour) failed every run on exactly this
    ramp-up, never on an actual excursion once cycling had begun. Samples
    before the first in-band arrival are excluded from the stray-temperature
    check; the relay-transition count is still checked over the whole run.
    A run that never arrives in band at all is still a FAIL, not a vacuous
    PASS from zero judged samples -- on/off cycling was never actually
    observed."""
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
    arrival_index = next(
        (i for i, t in enumerate(temps_c) if t is not None and abs(t - target_c) <= band), None
    )
    if arrival_index is None:
        return CaseResult(
            Verdict.FAIL,
            reason=f"never reached band (+/-{band:.1f}C of target {target_c:.1f}C)",
            observed={**observed, "transitions": transitions},
        )
    judged_temps = temps_c[arrival_index:]
    out_of_band = [t for t in judged_temps if t is not None and abs(t - target_c) > band]
    if out_of_band:
        return CaseResult(
            Verdict.FAIL,
            reason=f"{len(out_of_band)} sample(s) strayed more than {band:.1f}C from target {target_c:.1f}C "
            f"after first reaching band (sample {arrival_index})",
            observed={**observed, "out_of_band": out_of_band, "arrival_index": arrival_index},
        )
    return CaseResult(
        Verdict.PASS, observed={**observed, "transitions": transitions, "arrival_index": arrival_index}
    )


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


def parse_current_fault_sources(diag_text: str) -> "int | None":
    """Parses the Pico's own mirrored view of which ESP fault sources are
    currently asserted (`current_fault_sources`, only meaningful when
    `current_fault_sources_known` is set -- see mcp_server_safety.py's diag
    rendering). Used by HP-07 to confirm `escalate_guard_trip()`'s asserted
    fault line was actually released (`clear_this_runs_faults()`,
    profile_executor_relay_io.c) after the ack, distinct from the trip_reason/
    trip_mask latch itself."""
    m = re.search(r"current_fault_sources\s*[:=]?\s*(0x[0-9a-fA-F]+|\d+)", diag_text)
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


# ---------------------------------------------------------------------------
# TOTP password-reset suite (docs/TOTP_PASSWORD_RESET_PLAN.md sections 4/6a).
# Unit-tested against a fake board only -- see cases_totp.py's module
# docstring.
# ---------------------------------------------------------------------------

def judge_totp_status(data: Any) -> CaseResult:
    """TP-R01: GET /api/auth/totp_status (ROUTE_TIER_ADMIN, section 6a) must
    answer with a JSON object carrying a boolean `enrolled` field. Never
    surfaces any other key out of `data` -- section 6a promises this route
    carries no secret/seed/QR payload, and this judge would refuse to print
    one even if the board's JSON grew one later."""
    if not isinstance(data, dict) or not isinstance(data.get("enrolled"), bool):
        return CaseResult(
            Verdict.FAIL,
            # Key names only, never values: the docstring's promise not to
            # surface anything else out of `data` covers the FAIL path too.
            reason="GET /api/auth/totp_status response is missing a boolean 'enrolled' field",
            observed={"body_keys": sorted(data.keys()) if isinstance(data, dict) else None},
        )
    return CaseResult(Verdict.PASS, observed={"enrolled": data["enrolled"]})


def judge_totp_forgot_probe(status: Optional[int], body: Any) -> CaseResult:
    """TP-R02: POST /api/auth/forgot with a syntactically valid but wrong
    6-digit code. Per section 6a's always-202 anti-oracle contract, a
    well-formed request answers HTTP 202 with a `reset_token` field
    regardless of whether the code matched; a board whose clock is not
    SNTP-synced answers 503 (section 6a's one deliberate exception to
    always-202), and the shared login-ladder rate limit answers 429. Any
    other status is a contract violation."""
    if status == 202:
        token = body.get("reset_token") if isinstance(body, dict) else None
        if isinstance(token, str) and token:
            return CaseResult(Verdict.PASS, observed={"status": 202, "reset_token_present": True})
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/auth/forgot returned HTTP 202 but no reset_token in the body: {body!r}",
            observed={"status": 202},
        )
    if status in (503, 429):
        return CaseResult(Verdict.PASS, observed={"status": status})
    return CaseResult(
        Verdict.FAIL,
        reason=f"POST /api/auth/forgot returned unexpected HTTP {status}, expected 202/503/429",
        observed={"status": status},
    )


def judge_totp_open_tier(forgot_status: Optional[int], reset_status: Optional[int]) -> CaseResult:
    """TP-R03: both /api/auth/forgot and /api/auth/reset are ROUTE_TIER_OPEN
    (section 6a) -- the whole point of a forgot-password flow is that it
    works with no session. A 401/403 from either, sent with no session and
    no cookie, is a tier regression.

    A missing status (unreachable) or a 404 (route not flashed yet) on
    either route proves nothing about its tier, so it is INCONCLUSIVE,
    never a vacuous PASS. A 400/202/429/503 is a PASS: the route answered
    without demanding a session."""
    violations = []
    if forgot_status in (401, 403):
        violations.append(f"forgot={forgot_status}")
    if reset_status in (401, 403):
        violations.append(f"reset={reset_status}")
    if violations:
        return CaseResult(
            Verdict.FAIL,
            reason=f"OPEN-tier route(s) required a session with none presented: {', '.join(violations)}",
            observed={"forgot_status": forgot_status, "reset_status": reset_status},
        )
    unproven = [f"{name}={st}" for name, st in (("forgot", forgot_status), ("reset", reset_status))
                if st is None or st == 404]
    if unproven:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"could not observe an OPEN-tier answer (unreachable or route not flashed): {', '.join(unproven)}",
            observed={"forgot_status": forgot_status, "reset_status": reset_status},
        )
    return CaseResult(Verdict.PASS, observed={"forgot_status": forgot_status, "reset_status": reset_status})


def judge_totp_reset_roundtrip(
    forgot_status: Optional[int], reset_token_present: bool,
    reset_status: Optional[int], login_ok: bool,
) -> CaseResult:
    """TP-M01: the full forgot -> reset -> verify-by-login round trip.
    Mirrors `totp_reset_password()`'s own verification discipline
    (mcp_server_totp.py): a `{"ok": true}` reset response is never trusted
    alone -- only a real login with the new credential counts (CLAUDE.md's
    boot_guard write-lies section: an unverified success report is exactly
    the failure class this project has been bitten by before)."""
    if forgot_status != 202:
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/auth/forgot returned HTTP {forgot_status}, expected 202",
            observed={"forgot_status": forgot_status},
        )
    if not reset_token_present:
        return CaseResult(
            Verdict.FAIL,
            reason="POST /api/auth/forgot returned HTTP 202 but no reset_token in the body",
            observed={"forgot_status": forgot_status},
        )
    if reset_status != 200:
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/auth/reset returned HTTP {reset_status}, expected 200",
            observed={"forgot_status": forgot_status, "reset_status": reset_status},
        )
    if not login_ok:
        return CaseResult(
            Verdict.FAIL,
            reason="POST /api/auth/reset reported success but a login attempt with the new "
                   "password did not succeed -- do not trust the reset as complete",
            observed={"forgot_status": forgot_status, "reset_status": reset_status, "login_ok": False},
        )
    return CaseResult(
        Verdict.PASS,
        observed={"forgot_status": forgot_status, "reset_status": reset_status, "login_ok": True},
    )


#: Owner requirement 2026-10-01: at least 8 KB of internal RAM must stay free at runtime.
LCD_KEEP_MIN_FREE_INTERNAL_B = 8192


def judge_lcd_keep_discard(origin_is_builtin: "bool | None", page_names: "set | None", page_truncated: bool,
                           confirm_seen: "bool | None", status_after: "dict | None",
                           orig_unchanged: "bool | None", stray_profile_ids: "list | None",
                           heap_internal_min_free: "int | None" = None) -> CaseResult:
    """LCD-25: the home "Keep?" button opens the PIN-gated decide page after an edited
    firing ends. The page must offer "Discard edit" and "Save as new ...", and
    "Overwrite original" exactly when the origin is not a built-in. Tapping Discard
    edit must raise a confirm dialog; confirming it must clear the working copy and
    leave the original profile and the profile list untouched. Read after the page was
    used, heap_internal min_free must be >= LCD_KEEP_MIN_FREE_INTERNAL_B (below: FAIL;
    unreadable: INCONCLUSIVE)."""
    observed = {"origin_is_builtin": origin_is_builtin, "heap_internal_min_free": heap_internal_min_free,
                "page_names": sorted(page_names) if page_names is not None else None,
                "page_truncated": page_truncated, "confirm_seen": confirm_seen,
                "status_after": status_after, "orig_unchanged": orig_unchanged,
                "stray_profile_ids": stray_profile_ids}
    problems = []
    unknowns = []
    # Names are judged for ABSENCE only on a non-truncated, non-None read: an unreadable
    # or partial list proves nothing about a button being missing.
    page_readable = page_names is not None and not page_truncated
    names = page_names if page_names is not None else set()
    if page_readable:
        if not any(n == "Discard edit" for n in names):
            problems.append("the decide page has no 'Discard edit' button")
        if not any(str(n).startswith("Save as new") for n in names):
            problems.append("the decide page has no 'Save as new' button")
    else:
        unknowns.append("the decide page's tap targets were " + (
            "unreadable" if page_names is None else "truncated") + ", so button presence was not judged")
    if origin_is_builtin is None:
        unknowns.append("live status did not report origin_is_builtin")
    elif page_readable:
        has_overwrite = "Overwrite original" in names
        if has_overwrite == bool(origin_is_builtin):
            problems.append("'Overwrite original' is " + ("offered" if has_overwrite else "missing")
                            + f" but the origin is {'a built-in' if origin_is_builtin else 'a user profile'}")
    if confirm_seen is False:
        problems.append("tapping 'Discard edit' raised no confirm dialog")
    elif confirm_seen is None:
        # Discard edit was never tapped (page lacked it, or the read was unusable).
        unknowns.append("'Discard edit' was never tapped, so the confirm dialog was not exercised")
    if status_after is None:
        if problems:
            return CaseResult(Verdict.FAIL, observed=observed, reason="; ".join(problems))
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                          reason="could not read live status after the discard")
    if _lcd_edit_wid(status_after) >= 0:
        problems.append("a working copy remains after the LCD discard")
    if status_after.get("pending_decision"):
        problems.append("pending_decision is still true after the LCD discard")
    if orig_unchanged is False:
        problems.append("the original profile changed")
    if stray_profile_ids:
        problems.append(f"unexpected new profile ids {sorted(stray_profile_ids)}")
    heap_ok = isinstance(heap_internal_min_free, int) and not isinstance(heap_internal_min_free, bool)
    if heap_ok and heap_internal_min_free < LCD_KEEP_MIN_FREE_INTERNAL_B:
        problems.append(f"heap_internal min_free {heap_internal_min_free} B is below the "
                        f"{LCD_KEEP_MIN_FREE_INTERNAL_B} B floor after using the decide page")
    if problems:
        return CaseResult(Verdict.FAIL, observed=observed, reason="; ".join(problems))
    if unknowns:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason="; ".join(unknowns))
    if orig_unchanged is None:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                          reason="could not re-read the original profile to confirm it is unchanged")
    if not heap_ok:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                          reason="could not read heap_internal min_free after using the decide page")
    return CaseResult(Verdict.PASS, observed=observed)


def judge_web_zone_graphic(status: Optional[int], body: Any) -> CaseResult:
    """WEB-ZONE-14: the inputs ``zones_page.html``'s zone graphic is drawn
    from (docs/ZONE_GRAPHIC_PLAN.md section 4) are well-formed and agree with
    each other. The graphic is generated in the browser from ``GET
    /api/zones`` alone, so this judges that one response.

    Fail-closed in the plan's own spirit: every missing, non-integer or
    out-of-range field is a FAIL naming it, never skipped. An UNSET (0)
    relay device type on a relay no zone owns is a legal, honest state (the
    graphic renders the unknown glyph), so it is reported in ``observed`` and
    never fails."""
    if status != 200 or not isinstance(body, dict):
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET /api/zones failed or did not return a JSON object (status={status})",
            observed={"status": status},
        )

    def _int(v: Any) -> bool:
        return isinstance(v, int) and not isinstance(v, bool)

    problems: List[str] = []
    tc = body.get("thermo_count")
    if not _int(tc) or not 0 <= tc <= 3:
        return CaseResult(
            Verdict.FAIL,
            reason=f"thermo_count {tc!r} is not an integer in 0..3 (ring count)",
            observed={"thermo_count": tc},
        )
    if tc == 0:
        # A legitimately saved empty config (zones_config_accessors.h), not a fault.
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason="no zones configured (thermo_count 0); nothing to draw",
            observed={"thermo_count": tc},
        )
    zones = body.get("zones")
    if not isinstance(zones, list) or len(zones) < tc:
        return CaseResult(
            Verdict.FAIL,
            reason=(f"zones[] has {len(zones) if isinstance(zones, list) else repr(zones)} entries, "
                    f"fewer than thermo_count {tc}"),
            observed={"thermo_count": tc},
        )
    owned = 0
    zsummary = []
    no_tc_zones: List[int] = []
    for i in range(tc):
        z = zones[i]
        if not isinstance(z, dict):
            problems.append(f"zones[{i}] is not an object")
            continue
        for key, lo, hi in (("relay_mask", 0, 15), ("thermo_mask", 0, 7),
                            ("ct_mask", 0, 7), ("zone_type", 0, 1)):
            v = z.get(key)
            if not _int(v) or not lo <= v <= hi:
                problems.append(f"zones[{i}].{key} {v!r} not an integer in {lo}..{hi}")
        if _int(z.get("relay_mask")):
            owned |= z["relay_mask"] & 0xF
        tm = z.get("thermo_mask")
        if _int(tm) and 0 <= tm <= 7:
            if tm & ~((1 << tc) - 1):
                problems.append(
                    f"zones[{i}].thermo_mask {tm} has a bit outside thermo_count {tc} "
                    "(the POST's own rule)"
                )
            if tm == 0:
                no_tc_zones.append(i)
        zsummary.append({k: z.get(k) for k in ("relay_mask", "thermo_mask", "ct_mask", "zone_type")})
    declared = body.get("relay_zone_owned_mask")
    if not _int(declared):
        problems.append(f"relay_zone_owned_mask {declared!r} is not an integer")
    elif declared != owned:
        problems.append(
            f"relay_zone_owned_mask {declared} != union of zones' relay_mask {owned} "
            "(extra-relay derivation disagrees between server and zones[])"
        )
    names = body.get("relay_names")
    types = body.get("relay_types")
    if not isinstance(names, list) or len(names) != 4 or not all(isinstance(n, str) for n in names):
        problems.append(f"relay_names {names!r} is not a list of 4 strings")
    if not isinstance(types, list) or len(types) != 4 or not all(_int(t) and 0 <= t <= 6 for t in types):
        problems.append(f"relay_types {types!r} is not a list of 4 integers in 0..6")
    observed = {
        "thermo_count": tc, "zones": zsummary, "relay_zone_owned_mask": declared,
        "relay_types": types, "zones_without_thermocouple": no_tc_zones,
    }
    if problems:
        return CaseResult(Verdict.FAIL, reason="; ".join(problems), observed=observed)
    observed["unset_unowned_relays"] = [
        r + 1 for r in range(4) if not (owned >> r) & 1 and types[r] == 0
    ]
    return CaseResult(Verdict.PASS, observed=observed)
