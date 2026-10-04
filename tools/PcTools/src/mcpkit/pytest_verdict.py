"""Pure parsing of pytest output, so a silently lost xdist worker can't read as a pass.

Observed twice: a full ``-n 4`` run of the PcTools suite printed
``node down: Not properly terminated`` (a worker killed externally, probably
memory pressure from parallel sessions) and still ended with a green summary
and exit 0 -- the tests that worker owned simply never ran. Two independent
guards, both applied to the captured output regardless of exit status:

* a worker-loss marker anywhere in the output is a hard failure;
* the number of items collected must not exceed the sum of every outcome in
  the final summary line (a shortfall means tests vanished).

Everything here is a pure function over a string; the callers
(``mcpkit.workbench.run_pctools_tests``, ``tools/regression_suite.py``) own the
subprocess.
"""

from __future__ import annotations

import re
from typing import List, Optional

#: Substring of the skip reason conftest.py gives ``@pytest.mark.slow`` tests
#: when KILNCTL_SLOW_TESTS is not 1. Runners need ``-rs`` so skip reasons show.
SLOW_SKIP_MARKER = "KILNCTL_SLOW_TESTS"

#: Per-test timeout (seconds) for PcTools runs. Must stay below both runners'
#: whole-run timeout (600 s) or it can never fire. The slowest known test,
#: test_ramp_assist_cone_scale.py, takes 20 to 100 s under load.
PER_TEST_TIMEOUT_S = 300

_WORKER_LOSS = re.compile(
    r"node down"
    r"|replacing crashed worker"
    r"|worker\b[^\n]*\bcrashed"
    r"|crashed worker",
    re.IGNORECASE,
)

# "collected 12 items", "collected 12 items / 3 deselected / 9 selected",
# "12 tests collected" (older/-q forms), xdist's "4 workers [12 items]".
_COLLECTED = re.compile(r"collected\s+(\d+)\s+items?(?:\s*/[^\n]*?\b(\d+)\s+selected)?", re.IGNORECASE)
_TESTS_COLLECTED = re.compile(r"(\d+)\s+tests?\s+collected(?:\s*\([^)]*?\b(\d+)\s+selected[^)]*\))?", re.IGNORECASE)
_XDIST_ITEMS = re.compile(r"\d+\s+workers?\s+\[(\d+)\s+items?\]", re.IGNORECASE)

_OUTCOME = re.compile(r"(\d+)\s+(passed|failed|skipped|xfailed|xpassed|errors?)\b")
_SUMMARY_TIME = re.compile(r"\bin\s+\d+(?:\.\d+)?s\b")


def worker_loss_lines(output: str) -> List[str]:
    """Every output line that reports a lost/crashed xdist worker."""
    return [ln.strip() for ln in output.splitlines() if _WORKER_LOSS.search(ln)]


def collected_count(output: str) -> Optional[int]:
    """Items that were selected to run, or None if no count is present.

    Prefers xdist's ``N workers [M items]`` (already post-deselect), then
    ``collected N items [/ ... K selected]``, then ``N tests collected``.
    """
    m = _XDIST_ITEMS.search(output)
    if m:
        return int(m.group(1))
    m = _COLLECTED.search(output)
    if m:
        return int(m.group(2) if m.group(2) else m.group(1))
    m = _TESTS_COLLECTED.search(output)
    if m:
        return int(m.group(2) if m.group(2) else m.group(1))
    return None


def summary_outcome_total(output: str) -> Optional[int]:
    """passed+failed+skipped+xfailed+xpassed+error from the final summary line.

    The final summary is the LAST line that carries both outcome counts (or
    ``no tests ran``) and an ``in N.NNs`` duration. None if there is none.
    """
    for ln in reversed(output.splitlines()):
        if not _SUMMARY_TIME.search(ln):
            continue
        found = _OUTCOME.findall(ln)
        if found:
            return sum(int(n) for n, _ in found)
        if "no tests ran" in ln:
            return 0
    return None


def pytest_output_problems(output: str) -> List[str]:
    """Reasons this pytest output must NOT be trusted as a pass (empty = clean).

    Meant to be applied even when pytest exited 0 and its summary shows
    ``0 failed``.
    """
    problems: List[str] = []
    lost = worker_loss_lines(output)
    if lost:
        problems.append(
            "xdist worker lost (tests it owned did not run): " + " | ".join(lost[:3]))

    if SLOW_SKIP_MARKER in output:
        problems.append(
            f"slow tests were skipped ({SLOW_SKIP_MARKER}=1 was not in effect): "
            "the standing run must execute them")

    collected = collected_count(output)
    total = summary_outcome_total(output)
    if total is None:
        problems.append("no pytest summary line found (run did not finish normally)")
    elif collected is None:
        problems.append("could not find a collected-item count to compare against the summary")
    elif total < collected:
        problems.append(
            f"test shortfall: collected {collected} but the summary accounts for only "
            f"{total} (passed+failed+skipped+xfailed+xpassed+error); "
            f"{collected - total} test(s) never reported")
    return problems
