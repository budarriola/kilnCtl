#!/usr/bin/env python3
"""Tests for kilnsim.testmgr -- the `kilnsim testmgr` tiered regression-suite
orchestrator. No hardware: presence detection is exercised via injected fake
probes (never a real SerialSimLink/UartLink), and scenario execution is
exercised against :class:`~kilnsim.link.MockSimLink`.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import report as report_mod  # noqa: E402
from kilnsim import testmgr as tm  # noqa: E402
from kilnsim.link import MockSimLink, SimLinkError  # noqa: E402
from kilnsim.scenario import load_scenario_text  # noqa: E402


# ---------------------------------------------------------------------------
# scenario fixtures -- small, hand-written, deliberately covering each tier
# ---------------------------------------------------------------------------
_TIER0_YAML = """
name: tier0_fixture_only
version: 1
exercises: []
seed: 1
timescale: 1
dut:
  profile: none
faults: []
expect:
  - name: estop_stays_healthy
    forbid: { dut: estop_open, before: { sim_end: true } }
"""

_TIER1_YAML = """
name: tier1_needs_saftyfw
version: 1
exercises: [S3]
seed: 2
timescale: 1
dut:
  profile: none
faults: []
expect:
  - name: k4_closes
    at_end: { dut: K4_closed }
"""

_TIER2_YAML = """
name: tier2_needs_esp
version: 1
exercises: [S6a]
seed: 3
timescale: 1
dut:
  profile: none
faults: []
expect:
  - name: fault_line_seen
    at_end: { dut: fault_line_asserted }
"""

_RUNNER_GAP_YAML = """
name: gap_guard_trip
version: 1
exercises: [S2]
seed: 4
timescale: 1
dut:
  profile: none
faults: []
expect:
  - name: never_trips
    forbid: { event: { type: guard_trip }, before: { sim_end: true } }
"""

_OPERATOR_ENABLE_YAML = """
name: needs_operator_enable
version: 1
exercises: []
seed: 5
timescale: 1
dut:
  profile: none
  operator_actions:
    - { at_sim_time: 0, action: request_enable }
faults: []
expect:
  - name: never_estops
    forbid: { dut: estop_open, before: { sim_end: true } }
"""

_TIMED_RELAY_YAML = """
name: needs_timed_relay
version: 1
exercises: []
seed: 6
timescale: 1
dut:
  profile: none
  operator_actions:
    - { at_sim_time: 0, action: request_enable }
    - { at_sim_time: 5, action: command_relay, relay: K1, state: closed }
faults: []
expect:
  - name: never_estops
    forbid: { dut: estop_open, before: { sim_end: true } }
"""


def _load(text: str):
    return load_scenario_text(text)


# ---------------------------------------------------------------------------
# classify_scenario / _walk_refs / _ref_tier
# ---------------------------------------------------------------------------
class ClassifyScenarioTests(unittest.TestCase):
    def test_tier0_scenario_needs_only_fixture(self):
        req = tm.classify_scenario(_load(_TIER0_YAML))
        self.assertEqual(req.min_tier, 0)
        self.assertFalse(req.has_runner_gap)

    def test_tier1_scenario_needs_saftyfw(self):
        req = tm.classify_scenario(_load(_TIER1_YAML))
        self.assertEqual(req.min_tier, 1)
        self.assertFalse(req.has_runner_gap)

    def test_tier2_scenario_needs_esp(self):
        req = tm.classify_scenario(_load(_TIER2_YAML))
        self.assertEqual(req.min_tier, 2)

    def test_guard_trip_clause_is_flagged_as_runner_gap(self):
        req = tm.classify_scenario(_load(_RUNNER_GAP_YAML))
        self.assertTrue(req.has_runner_gap)
        gap_refs = [r for r in req.refs if r[4]]
        self.assertTrue(any(r[2] == "guard_trip" for r in gap_refs))

    def test_operator_enable_action_requires_tier1(self):
        req = tm.classify_scenario(_load(_OPERATOR_ENABLE_YAML))
        self.assertEqual(req.min_tier, 1)

    def test_timed_relay_actions_flagged_separately(self):
        req = tm.classify_scenario(_load(_TIMED_RELAY_YAML))
        self.assertTrue(req.has_timed_relay_actions)
        # the tier0 fixture doesn't request a timed relay action
        req0 = tm.classify_scenario(_load(_OPERATOR_ENABLE_YAML))
        self.assertFalse(req0.has_timed_relay_actions)

    def test_unknown_dut_entity_defaults_conservatively_to_tier1(self):
        req = tm.classify_scenario(_load("""
name: unknown_entity
version: 1
exercises: []
faults: []
expect:
  - name: x
    at_end: { dut: SOME_NEW_THING_open }
"""))
        self.assertEqual(req.min_tier, 1)

    def test_real_scenario_library_classifies_without_raising(self):
        """Every shipped scenario should classify cleanly -- a crash here
        would mean the generic _walk_refs/_clause_raw couldn't handle a real
        clause shape."""
        scenarios, errors = tm.discover_scenarios(tm.default_scenarios_dir())
        self.assertEqual(errors, [], "a shipped scenario failed to load")
        self.assertGreater(len(scenarios), 20)
        for s in scenarios:
            req = tm.classify_scenario(s)
            self.assertIn(req.min_tier, (0, 1, 2))


# ---------------------------------------------------------------------------
# describe_runner_gap -- the loud-vacuity note shared by testmgr.
# run_one_scenario and kilnsim.cli.cmd_run (kilnsim run), so `kilnsim run`
# on a single guard_trip/guard_warn/link_up/trip_ineffective_latched
# scenario carries the same warning the full testmgr suite already gives,
# instead of a silent, indistinguishable-from-real clean PASS.
# ---------------------------------------------------------------------------
class DescribeRunnerGapTests(unittest.TestCase):
    def test_no_note_when_scenario_has_no_runner_gap(self):
        req = tm.classify_scenario(_load(_TIER0_YAML))
        self.assertIsNone(tm.describe_runner_gap(req, report_mod.PASS))

    def test_no_note_when_verdict_is_fail_even_with_a_gap(self):
        # A genuine FAIL is not vacuous -- the note exists to stop a CLEAN
        # verdict from being mistaken for hardware evidence, not to annotate
        # every run of a gappy scenario regardless of outcome.
        req = tm.classify_scenario(_load(_RUNNER_GAP_YAML))
        self.assertIsNone(tm.describe_runner_gap(req, report_mod.FAIL))

    def test_note_present_for_pass_verdict_on_a_gappy_scenario(self):
        req = tm.classify_scenario(_load(_RUNNER_GAP_YAML))
        note = tm.describe_runner_gap(req, report_mod.PASS)
        self.assertIsNotNone(note)
        self.assertIn("guard_trip", note)
        self.assertIn("PASS", note)
        self.assertIn("kilnsim.runner cannot produce", note)

    def test_note_present_for_skipped_verdict_too(self):
        req = tm.classify_scenario(_load(_RUNNER_GAP_YAML))
        note = tm.describe_runner_gap(req, report_mod.SKIPPED)
        self.assertIsNotNone(note)

    def test_run_one_scenario_detail_carries_the_same_note(self):
        """The suite-level path (run_one_scenario) and the single-scenario
        path (describe_runner_gap, used directly by kilnsim.cli.cmd_run)
        must not diverge in wording -- both are built from the same
        function now, this pins that down rather than asserting it once and
        letting the two drift apart silently."""
        scenario = _load(_RUNNER_GAP_YAML)
        link = MockSimLink()
        link.connect()
        presence = tm.HardwarePresence(
            fixture=tm.PresenceResult(True, "ok"),
            saftyfw=tm.PresenceResult(True, "ok"),
            esp=tm.PresenceResult(True, "ok"),
        )
        outcome = tm.run_one_scenario(link, scenario, presence, mock=True)
        req = tm.classify_scenario(scenario)
        standalone_note = tm.describe_runner_gap(req, outcome.verdict)
        if standalone_note is not None:
            self.assertIn(standalone_note, outcome.detail)


# ---------------------------------------------------------------------------
# presence detection
# ---------------------------------------------------------------------------
class PresenceTests(unittest.TestCase):
    def test_max_tier_staircase(self):
        present = tm.PresenceResult(True, "ok")
        absent = tm.PresenceResult(False, "no")
        self.assertEqual(tm.HardwarePresence(absent, absent, absent).max_tier, -1)
        self.assertEqual(tm.HardwarePresence(present, absent, absent).max_tier, 0)
        self.assertEqual(tm.HardwarePresence(present, present, absent).max_tier, 1)
        self.assertEqual(tm.HardwarePresence(present, present, present).max_tier, 2)

    def test_fixture_present_but_saftyfw_absent_caps_at_tier0(self):
        # even if ESP were somehow reported present with SaftyFW absent, the
        # staircase must not let that leak past tier 0 -- SaftyFW is the
        # gate for tier 1 and everything above it.
        p = tm.HardwarePresence(
            tm.PresenceResult(True, "fixture ok"),
            tm.PresenceResult(False, "no saftyfw"),
            tm.PresenceResult(True, "esp somehow ok"),
        )
        self.assertEqual(p.max_tier, 0)

    def test_default_fixture_probe_success(self):
        link = MockSimLink()
        result = tm.default_fixture_probe(link)
        self.assertTrue(result.present)
        self.assertTrue(link.is_connected)

    def test_default_fixture_probe_connect_failure(self):
        class _BrokenLink(MockSimLink):
            def connect(self, port=None):
                raise SimLinkError("simulated: port not found")

        result = tm.default_fixture_probe(_BrokenLink())
        self.assertFalse(result.present)
        self.assertIn("simulated: port not found", result.detail)

    def test_default_fixture_probe_bad_ping_reply(self):
        link = MockSimLink()
        from kilnsim.protocol import CommandGroup, SysCmd

        link.script_response(CommandGroup.SYS, SysCmd.PING, {"pong": False})
        result = tm.default_fixture_probe(link)
        self.assertFalse(result.present)
        self.assertIn("did not answer", result.detail)


# ---------------------------------------------------------------------------
# run_one_scenario
# ---------------------------------------------------------------------------
class RunOneScenarioTests(unittest.TestCase):
    def _presence(self, max_tier: int) -> tm.HardwarePresence:
        present = tm.PresenceResult(True, "ok")
        absent = tm.PresenceResult(False, "no")
        if max_tier == -1:
            return tm.HardwarePresence(absent, absent, absent)
        if max_tier == 0:
            return tm.HardwarePresence(present, absent, absent)
        if max_tier == 1:
            return tm.HardwarePresence(present, present, absent)
        return tm.HardwarePresence(present, present, present)

    def test_not_runnable_when_tier_unmet(self):
        link = MockSimLink()
        link.connect()
        scenario = _load(_TIER2_YAML)  # needs tier 2
        outcome = tm.run_one_scenario(link, scenario, self._presence(0), mock=True)
        self.assertEqual(outcome.verdict, tm.NOT_RUNNABLE)
        self.assertIn("tier 2", outcome.detail)
        self.assertNotEqual(outcome.verdict, report_mod.FAIL)

    def test_runs_and_passes_when_tier_met(self):
        link = MockSimLink()
        link.connect()
        scenario = _load(_TIER0_YAML)
        outcome = tm.run_one_scenario(link, scenario, self._presence(0), mock=True)
        self.assertEqual(outcome.verdict, report_mod.PASS)
        self.assertIsNotNone(outcome.report)

    def test_reset_sim_error_becomes_error_outcome(self):
        link = MockSimLink()
        link.connect()
        from kilnsim.protocol import CommandGroup, SysCmd

        link.script_response(CommandGroup.SYS, SysCmd.RESET_SIM, {}, error="simulated: reset refused")
        outcome = tm.run_one_scenario(link, _load(_TIER0_YAML), self._presence(0), mock=True)
        self.assertEqual(outcome.verdict, tm.ERROR)
        self.assertIn("simulated: reset refused", outcome.detail)

    def test_reset_sim_is_sent_before_every_run(self):
        link = MockSimLink()
        link.connect()
        tm.run_one_scenario(link, _load(_TIER0_YAML), self._presence(0), mock=True)
        from kilnsim.protocol import CommandGroup, SysCmd

        reset_calls = [c for c in link.sent_commands if c[0] == CommandGroup.SYS and c[1] == SysCmd.RESET_SIM]
        self.assertEqual(len(reset_calls), 1)

    def test_runner_gap_annotated_even_on_overall_pass(self):
        link = MockSimLink()
        link.connect()
        outcome = tm.run_one_scenario(link, _load(_RUNNER_GAP_YAML), self._presence(1), mock=True)
        # no guard_trip event was ever injected, so the forbid clause PASSes
        # (never observed) -- but it must still carry the runner-gap caveat.
        self.assertEqual(outcome.verdict, report_mod.PASS)
        self.assertTrue(outcome.has_runner_gap)
        self.assertIn("kilnsim.runner cannot produce", outcome.detail)

    def test_operator_enable_invoked_when_scenario_wants_it(self):
        link = MockSimLink()
        link.connect()
        calls = []
        outcome = tm.run_one_scenario(
            link, _load(_OPERATOR_ENABLE_YAML), self._presence(1), mock=True,
            request_enable_fn=lambda: calls.append(1),
        )
        self.assertEqual(calls, [1])
        self.assertIn("standing in for the operator", outcome.detail)

    def test_operator_enable_not_invoked_when_scenario_does_not_want_it(self):
        link = MockSimLink()
        link.connect()
        calls = []
        tm.run_one_scenario(
            link, _load(_TIER0_YAML), self._presence(1), mock=True,
            request_enable_fn=lambda: calls.append(1),
        )
        self.assertEqual(calls, [])

    def test_timed_relay_actions_flagged_in_detail_when_enable_runs(self):
        link = MockSimLink()
        link.connect()
        outcome = tm.run_one_scenario(
            link, _load(_TIMED_RELAY_YAML), self._presence(1), mock=True,
            request_enable_fn=lambda: None,
        )
        self.assertIn("does NOT replay", outcome.detail)


# ---------------------------------------------------------------------------
# discover_scenarios / select_quick_scenarios
# ---------------------------------------------------------------------------
class DiscoverAndQuickTests(unittest.TestCase):
    def test_malformed_yaml_becomes_error_outcome_not_silently_dropped(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            bad = Path(d) / "bad.yaml"
            bad.write_text("name: missing_version\n")  # 'version' is required
            good = Path(d) / "good.yaml"
            good.write_text(_TIER0_YAML)
            scenarios, errors = tm.discover_scenarios(Path(d))
        self.assertEqual(len(scenarios), 1)
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0].verdict, tm.ERROR)
        self.assertIn("could not load scenario", errors[0].detail)

    def test_quick_selects_shortest_estimated_scenarios(self):
        scenarios, _ = tm.discover_scenarios(tm.default_scenarios_dir())
        quick = tm.select_quick_scenarios(scenarios, limit=3)
        self.assertEqual(len(quick), 3)
        from kilnsim.runner import estimate_run_duration_s

        durations = [estimate_run_duration_s(s) for s in quick]
        self.assertEqual(durations, sorted(durations))
        # every non-selected scenario's estimate is >= the selected ones'
        rest = [s for s in scenarios if s.name not in {q.name for q in quick}]
        self.assertTrue(all(estimate_run_duration_s(r) >= durations[-1] for r in rest))

    def test_quick_limit_respected(self):
        scenarios, _ = tm.discover_scenarios(tm.default_scenarios_dir())
        self.assertEqual(len(tm.select_quick_scenarios(scenarios, limit=1)), 1)


# ---------------------------------------------------------------------------
# guard coverage
# ---------------------------------------------------------------------------
class GuardCoverageTests(unittest.TestCase):
    def _make_outcome(self, name, exercises, verdict, has_gap=False):
        return tm.ScenarioOutcome(
            name=name, path=name, exercises=exercises, min_tier=1,
            has_runner_gap=has_gap, verdict=verdict,
        )

    def _scenario_stub(self, name, exercises):
        return _load(f"""
name: {name}
version: 1
exercises: {exercises}
faults: []
expect: []
""")

    def test_no_scenario_declares_guard(self):
        cov = tm.compute_guard_coverage([], [])
        self.assertIn("no scenario declares", cov["S1"].status)

    def test_clean_pass_gives_hardware_evidence(self):
        scenarios = [self._scenario_stub("s_a", "[S3]")]
        outcomes = [self._make_outcome("s_a", ["S3"], report_mod.PASS)]
        cov = tm.compute_guard_coverage(scenarios, outcomes)
        self.assertIn("hardware evidence obtained", cov["S3"].status)

    def test_runner_gap_only_does_not_count_as_evidence(self):
        scenarios = [self._scenario_stub("s_b", "[S2]")]
        outcomes = [self._make_outcome("s_b", ["S2"], report_mod.PASS, has_gap=True)]
        cov = tm.compute_guard_coverage(scenarios, outcomes)
        self.assertNotIn("hardware evidence obtained", cov["S2"].status)
        self.assertIn("no usable evidence", cov["S2"].status)

    def test_not_runnable_reported_distinctly_from_no_evidence(self):
        scenarios = [self._scenario_stub("s_c", "[S6a]")]
        outcomes = [self._make_outcome("s_c", ["S6a"], tm.NOT_RUNNABLE)]
        cov = tm.compute_guard_coverage(scenarios, outcomes)
        self.assertIn("hardware tier unavailable", cov["S6a"].status)

    def test_declared_but_never_run(self):
        scenarios = [self._scenario_stub("s_d", "[S11]")]
        cov = tm.compute_guard_coverage(scenarios, [])
        self.assertIn("not exercised this session", cov["S11"].status)


# ---------------------------------------------------------------------------
# SuiteReport.exit_code
# ---------------------------------------------------------------------------
class ExitCodeTests(unittest.TestCase):
    def _report(self, selftest_passed, verdicts):
        selftest = None
        if selftest_passed is not None:
            from kilnsim import selftest as st

            selftest = st.SelftestReport(checks=[
                st.CheckResult("x", st.STATUS_PASS if selftest_passed else st.STATUS_FAIL)
            ])
        outcomes = [
            tm.ScenarioOutcome(name=f"s{i}", path="p", exercises=[], min_tier=0,
                                has_runner_gap=False, verdict=v)
            for i, v in enumerate(verdicts)
        ]
        presence = tm.HardwarePresence(
            tm.PresenceResult(True, "ok"), tm.PresenceResult(True, "ok"), tm.PresenceResult(True, "ok")
        )
        return tm.SuiteReport(presence=presence, quick=False, selftest=selftest,
                               scenario_outcomes=outcomes, guard_coverage={},
                               started_at=0.0, duration_s=0.0)

    def test_all_pass_exit_0(self):
        self.assertEqual(self._report(True, [report_mod.PASS, report_mod.PASS]).exit_code, 0)

    def test_any_fail_exit_1(self):
        self.assertEqual(self._report(True, [report_mod.PASS, report_mod.FAIL]).exit_code, 1)

    def test_any_error_exit_1(self):
        self.assertEqual(self._report(True, [report_mod.PASS, tm.ERROR]).exit_code, 1)

    def test_blocked_only_exit_2(self):
        self.assertEqual(self._report(True, [report_mod.PASS, report_mod.BLOCKED]).exit_code, 2)

    def test_selftest_failure_exit_1_even_if_scenarios_all_pass(self):
        self.assertEqual(self._report(False, [report_mod.PASS]).exit_code, 1)

    def test_not_runnable_and_skipped_quick_do_not_fail_the_run(self):
        self.assertEqual(self._report(True, [tm.NOT_RUNNABLE, tm.SKIPPED_QUICK]).exit_code, 0)

    def test_fixture_absent_exits_1_even_with_nothing_failing(self):
        """A suite that ran nothing must not be green -- see run_suite's own
        test_fixture_absent_reports_not_runnable_never_fail for the real-
        hardware run that exposed this."""
        report = self._report(None, [tm.NOT_RUNNABLE, tm.NOT_RUNNABLE])
        # to_text() walks every guard, so this needs a real (if empty)
        # coverage map rather than the bare {} the helper builds.
        report.guard_coverage = tm.compute_guard_coverage([], [])
        report.presence = tm.HardwarePresence(
            tm.PresenceResult(False, "fixture not reachable"),
            tm.PresenceResult(False, "n/a"),
            tm.PresenceResult(False, "n/a"),
        )
        self.assertEqual(report.exit_code, 1)
        self.assertIn("fixture absent, nothing ran", report.to_text())

    def test_higher_tier_absent_still_exits_0(self):
        """The fixture is present and its own scenarios passed; SaftyFW and
        the ESP are not attached. That is a legitimate 0 -- the distinction
        the fixture-absent case above turns on."""
        report = self._report(True, [report_mod.PASS, tm.NOT_RUNNABLE])
        report.presence = tm.HardwarePresence(
            tm.PresenceResult(True, "ok"),
            tm.PresenceResult(False, "no SaftyFW on the bench"),
            tm.PresenceResult(False, "no ESP on the bench"),
        )
        self.assertEqual(report.exit_code, 0)


# ---------------------------------------------------------------------------
# run_suite -- end-to-end orchestration against fakes
# ---------------------------------------------------------------------------
class RunSuiteTests(unittest.TestCase):
    def test_fixture_absent_reports_not_runnable_never_fail(self):
        link = MockSimLink()

        def fake_fixture_probe(link, port):
            return tm.PresenceResult(False, "simulated: no fixture on the bench")

        report = tm.run_suite(
            link, tm.default_scenarios_dir(),
            fixture_probe=fake_fixture_probe,
            esp_saftyfw_probe=lambda: (tm.PresenceResult(False, "n/a"), tm.PresenceResult(False, "n/a")),
        )
        self.assertFalse(report.presence.fixture.present)
        self.assertIsNone(report.selftest)
        self.assertGreater(len(report.scenario_outcomes), 20)
        self.assertTrue(all(o.verdict == tm.NOT_RUNNABLE for o in report.scenario_outcomes))
        # Every scenario is NOT_RUNNABLE rather than FAIL -- finding 1's whole
        # point, and unchanged: a positive assertion must not fail merely
        # because there was no DUT to observe.
        #
        # The exit code, however, is now 1, reversing this test's original
        # assertion of 0. Found by running the suite against real hardware for
        # the first time (2026-08-24) with the fixture's CDC port wedged: it
        # printed "exit code: 0 (PASS)" having run precisely nothing. "No
        # false FAIL when hardware isn't attached" is the right rule for a
        # higher TIER -- SaftyFW or the ESP missing still exits 0, covered by
        # test_higher_tier_absent_still_exits_0 below, because the
        # fixture-only scenarios genuinely ran and genuinely passed. The
        # fixture itself is the floor: without it there is no evidence of any
        # kind, and a green result is a lie about what was tested.
        self.assertEqual(report.exit_code, 1)

    def test_fixture_present_runs_selftest_and_quick_subset(self):
        link = MockSimLink()

        def fake_fixture_probe(link, port):
            return tm.default_fixture_probe(link, port)

        report = tm.run_suite(
            link, tm.default_scenarios_dir(),
            quick=True, mock=True,
            fixture_probe=fake_fixture_probe,
            esp_saftyfw_probe=lambda: (tm.PresenceResult(False, "n/a"), tm.PresenceResult(False, "n/a")),
        )
        self.assertTrue(report.presence.fixture.present)
        self.assertIsNotNone(report.selftest)
        ran = [o for o in report.scenario_outcomes if o.verdict not in (tm.NOT_RUNNABLE, tm.SKIPPED_QUICK, tm.ERROR)]
        skipped_quick = [o for o in report.scenario_outcomes if o.verdict == tm.SKIPPED_QUICK]
        self.assertGreater(len(skipped_quick), 0, "quick mode should skip most of the 27 scenarios")
        self.assertLessEqual(len(ran) + len([o for o in report.scenario_outcomes if o.verdict == tm.NOT_RUNNABLE]), 6)

    def test_mock_never_touches_the_real_esp_saftyfw_probe(self):
        """Regression: `kilnsim --mock testmgr` must never open a real
        kilnctrl UartLink. Caught for real while smoke-testing this module
        (default_esp_and_saftyfw_probe tried to open a real COM port during
        a --mock run before run_suite() special-cased mock=True)."""
        link = MockSimLink()
        calls = []
        orig = tm.default_esp_and_saftyfw_probe
        tm.default_esp_and_saftyfw_probe = lambda *a, **k: (calls.append(1) or (tm.PresenceResult(False, "x"), tm.PresenceResult(False, "x")))
        try:
            report = tm.run_suite(link, tm.default_scenarios_dir(), quick=True, mock=True)
        finally:
            tm.default_esp_and_saftyfw_probe = orig
        self.assertEqual(calls, [], "default_esp_and_saftyfw_probe (real kilnctrl I/O) was called during --mock")
        self.assertFalse(report.presence.esp.present)
        self.assertIn("--mock", report.presence.esp.detail)

    def test_to_dict_and_to_json_round_trip(self):
        link = MockSimLink()
        report = tm.run_suite(
            link, tm.default_scenarios_dir(), quick=True, mock=True,
            fixture_probe=lambda link, port: tm.default_fixture_probe(link, port),
            esp_saftyfw_probe=lambda: (tm.PresenceResult(False, "n/a"), tm.PresenceResult(False, "n/a")),
        )
        import json

        parsed = json.loads(report.to_json())
        self.assertIn("presence", parsed)
        self.assertIn("guard_coverage", parsed)
        self.assertEqual(parsed["exit_code"], report.exit_code)
        text = report.to_text()
        self.assertIn("hardware presence:", text)
        self.assertIn("guard coverage", text)


# ---------------------------------------------------------------------------
# guard observer wiring -- the task this pass closes: testmgr previously
# passed NO guard_observer on every scenario run, so every guard-typed
# expectation came back BLOCKED (kilnsim.runner's own
# _block_expectations_missing_guard_observer) and compute_guard_coverage
# could never credit a guard with real hardware evidence. These tests never
# touch a real kilnctrl/SaftyFW link -- run_one_scenario's real (non-mock)
# branch calls kilnsim.testmgr's own module-level `run_scenario` name, which
# is monkeypatched here to a tiny stub that only records what it was called
# with, exactly the same technique test_kilnsim_runner_guard_and_spi.py uses
# for its own _FakeGuardObserver (a duck-typed test double, no hardware).
# ---------------------------------------------------------------------------
class _StubReport:
    """The only two attributes run_one_scenario's real-run branch reads off
    a Report: `.verdict` (for the outcome and describe_runner_gap) and
    `.events` (never read on this path, but kept absent-safe -- accessing it
    would raise AttributeError, which is itself a useful signal if some
    future change starts reading more of the report than expected here)."""

    def __init__(self, verdict: str) -> None:
        self.verdict = verdict


class GuardObserverWiringTests(unittest.TestCase):
    def _presence(self, saftyfw_present: bool) -> tm.HardwarePresence:
        present = tm.PresenceResult(True, "ok")
        return tm.HardwarePresence(
            present,
            present if saftyfw_present else tm.PresenceResult(False, "no saftyfw"),
            tm.PresenceResult(False, "n/a"),
        )

    def test_run_one_scenario_passes_the_built_observer_to_run_scenario(self):
        link = MockSimLink()
        link.connect()
        scenario = _load(_TIER1_YAML)

        fake_observer = object()
        closed = []

        def fake_factory():
            return fake_observer, lambda: closed.append(1)

        captured = {}

        def fake_run_scenario(link_, scenario_, *, seed, timescale, guard_observer=None):
            captured["guard_observer"] = guard_observer
            return _StubReport(report_mod.PASS)

        orig = tm.run_scenario
        tm.run_scenario = fake_run_scenario
        try:
            outcome = tm.run_one_scenario(
                link, scenario, self._presence(True), mock=False,
                guard_observer_factory=fake_factory,
            )
        finally:
            tm.run_scenario = orig

        self.assertIs(captured.get("guard_observer"), fake_observer)
        self.assertTrue(outcome.guard_observer_attached)
        self.assertEqual(closed, [1], "close_fn must run even when the scenario itself succeeds")

    def test_run_one_scenario_closes_observer_even_when_run_scenario_raises(self):
        link = MockSimLink()
        link.connect()
        scenario = _load(_TIER1_YAML)
        closed = []

        def fake_factory():
            return object(), lambda: closed.append(1)

        def fake_run_scenario(link_, scenario_, *, seed, timescale, guard_observer=None):
            raise SimLinkError("simulated: link dropped mid-run")

        orig = tm.run_scenario
        tm.run_scenario = fake_run_scenario
        try:
            outcome = tm.run_one_scenario(
                link, scenario, self._presence(True), mock=False,
                guard_observer_factory=fake_factory,
            )
        finally:
            tm.run_scenario = orig

        self.assertEqual(outcome.verdict, tm.ERROR)
        self.assertEqual(closed, [1], "close_fn must run even when run_scenario raises")

    def test_run_one_scenario_never_attaches_when_no_factory_given(self):
        link = MockSimLink()
        link.connect()
        outcome = tm.run_one_scenario(link, _load(_TIER0_YAML), self._presence(True), mock=True)
        self.assertFalse(outcome.guard_observer_attached)

    def test_guard_observer_factory_failure_raises_guardobserverunavailable(self):
        link = MockSimLink()
        link.connect()

        def broken_factory():
            raise RuntimeError("simulated: kilnctrl UartLink.connect() failed")

        with self.assertRaises(tm.GuardObserverUnavailable) as ctx:
            tm.run_one_scenario(
                link, _load(_TIER1_YAML), self._presence(True), mock=False,
                guard_observer_factory=broken_factory,
            )
        self.assertIn("simulated: kilnctrl UartLink.connect() failed", str(ctx.exception))


# ---------------------------------------------------------------------------
# run_suite: guards=True's fail-loudly contract (requirement 2) and its
# reuse of the SAME presence/tier detection run_one_scenario already uses
# (requirement 4), never a second, parallel SaftyFW-reachability probe.
# ---------------------------------------------------------------------------
class RunSuiteGuardsTests(unittest.TestCase):
    def test_guards_with_mock_raises_before_touching_the_link(self):
        link = MockSimLink()
        probe_calls = []

        def spy_fixture_probe(link_, port):
            probe_calls.append(1)
            return tm.default_fixture_probe(link_, port)

        with self.assertRaises(tm.GuardObserverUnavailable) as ctx:
            tm.run_suite(
                link, tm.default_scenarios_dir(), quick=True, mock=True, guards=True,
                fixture_probe=spy_fixture_probe,
            )
        self.assertIn("--mock", str(ctx.exception))
        self.assertEqual(probe_calls, [], "must fail before even probing for the fixture")
        self.assertFalse(link.is_connected, "must fail before ever connecting the fixture link")

    def test_guards_without_saftyfw_present_raises(self):
        link = MockSimLink()
        with self.assertRaises(tm.GuardObserverUnavailable) as ctx:
            tm.run_suite(
                link, tm.default_scenarios_dir(), quick=True, mock=False, guards=True,
                fixture_probe=lambda link, port: tm.default_fixture_probe(link, port),
                esp_saftyfw_probe=lambda: (tm.PresenceResult(False, "n/a"), tm.PresenceResult(False, "no saftyfw")),
            )
        self.assertIn("SaftyFW is not reachable", str(ctx.exception))

    def test_guards_attaches_a_fresh_observer_per_scenario(self):
        """With SaftyFW present and guards=True, run_suite must call the
        guard_observer_factory once per scenario that actually runs (never
        shared across scenarios -- see run_one_scenario's own comment on
        stale edge-tracker state), and every such scenario's outcome must
        carry guard_observer_attached=True."""
        link = MockSimLink()
        factory_calls = []

        def fake_guard_observer_factory(port):
            factory_calls.append(port)
            return object(), lambda: None

        def fake_run_scenario(link_, scenario_, *, seed, timescale, guard_observer=None):
            return _StubReport(report_mod.PASS)

        orig = tm.run_scenario
        tm.run_scenario = fake_run_scenario
        try:
            report = tm.run_suite(
                link, tm.default_scenarios_dir(), quick=True, mock=False, guards=True, port="COMX",
                fixture_probe=lambda link, port: tm.default_fixture_probe(link, port),
                esp_saftyfw_probe=lambda: (tm.PresenceResult(True, "ok"), tm.PresenceResult(True, "ok")),
                request_enable_fn_factory=lambda port: None,
                guard_observer_factory=fake_guard_observer_factory,
            )
        finally:
            tm.run_scenario = orig

        ran = [o for o in report.scenario_outcomes if o.verdict not in (tm.NOT_RUNNABLE, tm.SKIPPED_QUICK, tm.ERROR)]
        self.assertGreater(len(ran), 0)
        self.assertEqual(len(factory_calls), len(ran))
        self.assertTrue(all(p == "COMX" for p in factory_calls))
        self.assertTrue(all(o.guard_observer_attached for o in ran))
        self.assertTrue(report.guards)


# ---------------------------------------------------------------------------
# guard coverage: distinguishing "observed, no trip seen" from "never
# observable -- no observer attached" (requirement 3). These two facts are
# genuinely different and must print differently, not collapse into the same
# "no usable evidence" bucket the pre-existing runner-gap-only status used.
# ---------------------------------------------------------------------------
class GuardCoverageObserverDistinctionTests(unittest.TestCase):
    def _make_outcome(self, name, exercises, verdict, has_gap=False, observer_attached=False):
        return tm.ScenarioOutcome(
            name=name, path=name, exercises=exercises, min_tier=1,
            has_runner_gap=has_gap, verdict=verdict,
            guard_observer_attached=observer_attached,
        )

    def _scenario_stub(self, name, exercises):
        return _load(f"""
name: {name}
version: 1
exercises: {exercises}
faults: []
expect: []
""")

    def test_gap_clause_with_no_observer_attached_is_never_observed(self):
        scenarios = [self._scenario_stub("s_e", "[S4]")]
        outcomes = [self._make_outcome("s_e", ["S4"], report_mod.PASS, has_gap=True, observer_attached=False)]
        cov = tm.compute_guard_coverage(scenarios, outcomes)
        self.assertIn("no usable evidence", cov["S4"].status)
        self.assertIn("no guard observer was attached", cov["S4"].detail[0])
        self.assertIn("NEVER OBSERVED", cov["S4"].detail[0])

    def test_gap_clause_with_observer_attached_and_clean_pass_counts_as_evidence(self):
        scenarios = [self._scenario_stub("s_f", "[S5]")]
        outcomes = [self._make_outcome("s_f", ["S5"], report_mod.PASS, has_gap=True, observer_attached=True)]
        cov = tm.compute_guard_coverage(scenarios, outcomes)
        self.assertIn("hardware evidence obtained", cov["S5"].status)

    def test_gap_clause_with_observer_attached_but_not_pass_is_distinguishable_from_no_observer(self):
        # SKIPPED here stands in for "the observer really watched, but the
        # triggering trip just never happened this run" -- a genuinely
        # different fact from "nothing was watching at all" (the previous
        # test in this class). Both fall short of "clean PASS", but their
        # detail lines must not read the same.
        scenarios = [self._scenario_stub("s_g", "[S7]")]
        outcomes = [self._make_outcome("s_g", ["S7"], report_mod.SKIPPED, has_gap=True, observer_attached=True)]
        cov = tm.compute_guard_coverage(scenarios, outcomes)
        self.assertNotIn("no usable evidence", cov["S7"].status)
        self.assertIn("did not produce a clean PASS", cov["S7"].status)
        self.assertIn("(guard observer attached)", cov["S7"].detail[0])
        self.assertNotIn("NEVER OBSERVED", cov["S7"].detail[0])


# ---------------------------------------------------------------------------
# kilnsim.cli wiring: --guards/--no-guards on `kilnsim testmgr`, and
# cmd_testmgr's loud-failure handling of GuardObserverUnavailable. Not
# test_kilnsim_cli.py (that file is owned by a concurrent session, task
# constraint) -- these live here since they exercise this task's own
# addition to kilnsim.cli.build_parser/cmd_testmgr.
# ---------------------------------------------------------------------------
class CliGuardsFlagTests(unittest.TestCase):
    def test_guards_defaults_to_false(self):
        from kilnsim.cli import build_parser

        args = build_parser().parse_args(["testmgr"])
        self.assertFalse(args.guards)

    def test_guards_flag_sets_true(self):
        from kilnsim.cli import build_parser

        args = build_parser().parse_args(["testmgr", "--guards"])
        self.assertTrue(args.guards)

    def test_no_guards_flag_is_explicit_false(self):
        from kilnsim.cli import build_parser

        args = build_parser().parse_args(["testmgr", "--no-guards"])
        self.assertFalse(args.guards)

    def test_cmd_testmgr_reports_guardobserverunavailable_loudly_not_a_traceback(self):
        from kilnsim.cli import build_parser, cmd_testmgr

        args = build_parser().parse_args(["--mock", "testmgr", "--guards", "--quick"])

        def _raise(*a, **k):
            raise tm.GuardObserverUnavailable("simulated: no SaftyFW this session")

        orig = tm.run_suite
        tm.run_suite = _raise
        try:
            rc = cmd_testmgr(args)
        finally:
            tm.run_suite = orig
        self.assertEqual(rc, 1, "a guard-observer-unavailable error must be a real failure, not exit 0")


if __name__ == "__main__":
    unittest.main()
