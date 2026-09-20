"""bench_test -- standardized, callable testing of the whole kilnCtl
controller on the bench (docs/BENCH_TEST_SYSTEM_PLAN.md).

Wave 0 (this package's first cut) ships the skeleton only: the case
registry, the run lifecycle (preflight/order/teardown), the summary.json +
transcript.md writer, and the read-only ``smoke`` suite (ST-05, FL-01..09,
SK-01/03/04, SP-01/02/05/07). Later waves (see the plan doc §8) add
cases_web.py, cases_heat.py, cases_safety.py, cases_lcd.py etc. -- none of
that exists yet, so `nightly`/`full` are declared in the registry but only
contain the cases wave 0 actually implements.

Everything here calls the *Python functions* the kilnctrl MCP tools wrap
(imported from the sibling `kilnctrl` modules), never a second MCP server
and never hardware directly -- see the plan doc §2.2. Cases are read-only
in wave 0: no case here heats, flashes, writes config, or touches Wi-Fi.
"""
from __future__ import annotations

from .registry import Verdict, CaseResult, CaseSpec, REGISTRY, SUITES, get_case, suite_case_ids
from . import cases_smoke as _cases_smoke  # noqa: F401 - import wires judge functions into REGISTRY
from . import cases_web as _cases_web  # noqa: F401 - import wires judge functions into REGISTRY
from .runner import BenchTestRunner, run_suite

__all__ = [
    "Verdict",
    "CaseResult",
    "CaseSpec",
    "REGISTRY",
    "SUITES",
    "get_case",
    "suite_case_ids",
    "BenchTestRunner",
    "run_suite",
]
