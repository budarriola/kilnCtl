"""bench_test -- standardized, callable testing of the whole kilnCtl
controller on the bench (docs/BENCH_TEST_SYSTEM_PLAN.md).

Wave 0 (this package's first cut) ships the skeleton only: the case
registry, the run lifecycle (preflight/order/teardown), the summary.json +
transcript.md writer, and the read-only ``smoke`` suite (ST-05, FL-01..09,
SK-01/03/04, SP-01/02/05/07). Wave 1a added ``cases_web.py`` (the 18 WEB
``-01`` render cases plus the generated tier sweep WEB-X-03); Wave 1c adds
``cases_lcd.py`` + ``lcd_sampler.py`` (LCD-01, LCD-08, LCD-21). Later waves
(see the plan doc §8) add cases_heat.py, cases_safety.py, the rest of the
``lcd`` and ``web`` suites, etc. -- ``nightly``/``full`` are declared in the
registry but only contain the cases implemented so far.

Everything here calls the *Python functions* the kilnctrl MCP tools wrap
(imported from the sibling `kilnctrl` modules), never a second MCP server
and never hardware directly -- see the plan doc §2.2. Cases are read-only
in wave 0: no case here heats, flashes, writes config, or touches Wi-Fi.
"""
from __future__ import annotations

from .registry import Verdict, CaseResult, CaseSpec, REGISTRY, SUITES, get_case, suite_case_ids
from . import cases_smoke as _cases_smoke  # noqa: F401 - import wires judge functions into REGISTRY
from . import cases_web as _cases_web  # noqa: F401 - import wires judge functions into REGISTRY
from . import cases_lcd as _cases_lcd  # noqa: F401 - import wires LCD-01/08/21 judge functions into REGISTRY
from . import cases_heat as _cases_heat  # noqa: F401 - import wires judge functions into REGISTRY
from . import cases_safety as _cases_safety  # noqa: F401 - import wires judge functions into REGISTRY
from . import cases_fl as _cases_fl  # noqa: F401 - import wires judge functions into REGISTRY
from . import cases_web_dash as _cases_web_dash  # noqa: F401 - WEB-DASH-02..12 judges
from . import cases_web_rw as _cases_web_rw  # noqa: F401 - import wires WEB read/write round-trip judge functions into REGISTRY
from . import cases_ota as _cases_ota  # noqa: F401 - import wires OTA judge functions into REGISTRY
from . import cases_autotune as _cases_autotune  # noqa: F401 - import wires AT-01..05 judge functions into REGISTRY
from . import cases_aux as _cases_aux  # noqa: F401 - import wires AX-C01..R01 judge functions into REGISTRY
from . import cases_totp as _cases_totp  # noqa: F401 - import wires TP-R01..M01 judge functions into REGISTRY
from . import cases_static as _cases_static  # noqa: F401 - import wires ST-01..04 judge functions into REGISTRY
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
