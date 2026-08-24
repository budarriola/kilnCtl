#!/usr/bin/env python3
"""Tests for kilnsim.selftest -- PLAN.md sec 13 layer 2's ``kilnsim
selftest``. No hardware: individual check functions are exercised against
:class:`~kilnsim.link.MockSimLink` (scripted where a specific reply shape
matters), and the report/aggregation logic (:class:`CheckResult`,
:class:`SelftestReport`, :func:`_run_check`) is tested directly since it has
no link dependency at all.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import selftest as st  # noqa: E402
from kilnsim.link import MockSimLink, SimLinkError  # noqa: E402
from kilnsim.protocol import CommandGroup, Event, EventType, FaultCmd, SysCmd  # noqa: E402


def _inject_after_schedule(link: MockSimLink, events) -> None:
    """The check drains any stale event backlog *before* it sends
    FAULT_SCHEDULE, so events meant to simulate that fault firing must be
    injected only once FAULT_SCHEDULE itself has actually been sent --
    otherwise the drain step consumes them before the check ever sees
    them."""
    orig_send = link.send_command

    def wrapped(group, cmd, payload=None, timeout=None):
        result = orig_send(group, cmd, payload, timeout=timeout)
        if group == CommandGroup.FAULT and cmd == FaultCmd.SCHEDULE:
            for evt in events:
                link.inject_event(evt)
        return result

    link.send_command = wrapped


def _inject_after_fire_now(link: MockSimLink, events) -> None:
    """Simulates the real firmware firing (and the EVT frame reaching the
    PC's link-level event buffer) *during* FAULT_FIRE_NOW's own round trip --
    i.e. before _check_event_sequence_continuity's read loop has started
    calling read_events() at all. This is the exact scenario the bench-day
    investigation's "listener window race" hypothesis worried about: on
    :class:`MockSimLink`, ``inject_event`` always lands in the same
    already-buffered list read_events() drains regardless of timing (mirrors
    the real SimLink's RX-thread buffering, kilnsim/link.py's
    _handle_broadcast()), so a check that is genuinely robust to this must
    still PASS."""
    orig_send = link.send_command

    def wrapped(group, cmd, payload=None, timeout=None):
        result = orig_send(group, cmd, payload, timeout=timeout)
        if group == CommandGroup.FAULT and cmd == FaultCmd.FIRE_NOW:
            for evt in events:
                link.inject_event(evt)
        return result

    link.send_command = wrapped


class CheckResultAndReportTests(unittest.TestCase):
    def test_check_result_to_dict_rounds_duration(self):
        r = st.CheckResult(name="x", status=st.STATUS_PASS, detail="ok", duration_s=1.23456)
        d = r.to_dict()
        self.assertEqual(d["name"], "x")
        self.assertEqual(d["status"], "PASS")
        self.assertEqual(d["duration_s"], 1.235)

    def test_report_passed_true_with_no_fails(self):
        report = st.SelftestReport(checks=[
            st.CheckResult("a", st.STATUS_PASS),
            st.CheckResult("b", st.STATUS_SKIP),
            st.CheckResult("c", st.STATUS_NOT_RUNNABLE),
        ])
        self.assertTrue(report.passed)

    def test_report_passed_false_with_any_fail(self):
        report = st.SelftestReport(checks=[
            st.CheckResult("a", st.STATUS_PASS),
            st.CheckResult("b", st.STATUS_FAIL),
        ])
        self.assertFalse(report.passed)

    def test_to_json_round_trips(self):
        import json

        report = st.SelftestReport(checks=[st.CheckResult("a", st.STATUS_PASS, "detail")])
        parsed = json.loads(report.to_json())
        self.assertEqual(parsed["passed"], True)
        self.assertEqual(parsed["checks"][0]["name"], "a")

    def test_to_text_mentions_every_check_and_verdict(self):
        report = st.SelftestReport(checks=[st.CheckResult("a", st.STATUS_PASS)])
        text = report.to_text()
        self.assertIn("a", text)
        self.assertIn("PASSED", text)


class RunCheckWrapperTests(unittest.TestCase):
    """_run_check must never let a check crash the whole selftest run."""

    def test_normal_result_passes_through(self):
        result = st._run_check("name", lambda link: (st.STATUS_PASS, "ok"), link=None)
        self.assertEqual(result.status, st.STATUS_PASS)
        self.assertEqual(result.detail, "ok")

    def test_sim_link_error_becomes_fail(self):
        def raiser(link):
            raise SimLinkError("boom")

        result = st._run_check("name", raiser, link=None)
        self.assertEqual(result.status, st.STATUS_FAIL)
        self.assertIn("boom", result.detail)

    def test_unexpected_exception_becomes_fail_not_a_crash(self):
        def raiser(link):
            raise ValueError("oops")

        result = st._run_check("name", raiser, link=None)
        self.assertEqual(result.status, st.STATUS_FAIL)
        self.assertIn("ValueError", result.detail)
        self.assertIn("oops", result.detail)


class IndividualCheckTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_ping_roundtrip_passes_against_mock(self):
        status, detail = st._check_ping_roundtrip(self.link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("3/3", detail)

    def test_version_and_caps_passes_against_mock_defaults(self):
        status, detail = st._check_version_and_caps(self.link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("protocol v1", detail)

    def test_version_and_caps_fails_on_bad_protocol_version(self):
        self.link.script_response(CommandGroup.SYS, SysCmd.GET_CAPS, {
            "protocol_version": 0, "min_compatible": 1,
            "zone_count_max": 4, "tc_channel_count": 4, "ct_channel_count": 3, "relay_count": 5,
        })
        status, detail = st._check_version_and_caps(self.link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("min_compatible", detail)

    def test_command_groups_reachable_against_mock_defaults(self):
        status, detail = st._check_command_groups_reachable(self.link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("all 11 probed command groups", detail)

    def test_command_groups_reachable_reports_specific_failures(self):
        self.link.script_response(CommandGroup.RELAY, 1, {}, error="simulated failure")
        status, detail = st._check_command_groups_reachable(self.link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("RELAY/GET_STATES", detail)

    def test_telemetry_cadence_skips_on_mock(self):
        # MockSimLink has no get_last_telemetry -- push telemetry isn't
        # simulated -- so this must SKIP, never FAIL or fabricate a PASS.
        status, detail = st._check_telemetry_cadence(self.link)
        self.assertEqual(status, st.STATUS_SKIP)
        self.assertIn("push telemetry", detail)

    def test_event_sequence_continuity_fails_honestly_on_mock(self):
        # MockSimLink's FAULT_SCHEDULE doesn't run a real fault engine, so
        # no FAULT_FIRED EVT frame is ever produced -- this must be an
        # honest FAIL, not a fabricated PASS.
        status, detail = st._check_event_sequence_continuity(self.link)
        self.assertEqual(status, st.STATUS_FAIL)

    def test_event_sequence_continuity_detects_seq_gap(self):
        _inject_after_schedule(self.link, [
            Event(seq=1, sim_time_us=0, event_type=EventType.FAULT_FIRED),
            Event(seq=5, sim_time_us=1000, event_type=EventType.FAULT_FIRED),
        ])
        status, detail = st._check_event_sequence_continuity(self.link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("non-consecutive", detail)

    def test_event_sequence_continuity_passes_with_consecutive_events(self):
        _inject_after_schedule(self.link, [
            Event(seq=1, sim_time_us=0, event_type=EventType.FAULT_FIRED),
            Event(seq=2, sim_time_us=1000, event_type=EventType.FAULT_FIRED),
        ])
        status, detail = st._check_event_sequence_continuity(self.link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("FAULT_FIRED", detail)

    def test_event_sequence_continuity_uses_manual_trigger_and_fire_now(self):
        # Bench-day fix: was AT_SIM_TIME(t=0.05) (a trigger time that's
        # essentially always already in the sim's past); now MANUAL +
        # explicit FAULT_FIRE_NOW, so the check drives the fire itself
        # instead of relying on an already-past absolute time being handled
        # correctly by the tick-evaluated path.
        _inject_after_schedule(self.link, [
            Event(seq=1, sim_time_us=0, event_type=EventType.FAULT_FIRED),
        ])
        st._check_event_sequence_continuity(self.link)
        sent = self.link.sent_commands
        schedule_calls = [p for g, c, p in sent if g == CommandGroup.FAULT and c == FaultCmd.SCHEDULE]
        self.assertEqual(len(schedule_calls), 1)
        self.assertEqual(schedule_calls[0]["trigger"], {"kind": "manual"})
        fire_now_calls = [p for g, c, p in sent if g == CommandGroup.FAULT and c == FaultCmd.FIRE_NOW]
        self.assertEqual(len(fire_now_calls), 1)
        self.assertEqual(fire_now_calls[0]["fault_slot"], 30)

    def test_event_sequence_continuity_survives_event_arriving_before_read_loop(self):
        # The bench-day "listener window race" hypothesis: an EVT frame that
        # arrives *during* FAULT_FIRE_NOW's own round trip, before the check's
        # read loop below has made its first read_events() call. Confirmed by
        # reading kilnsim/link.py that this can never actually lose an event
        # (the RX thread buffers into self._events regardless of whether a
        # reader is waiting) -- this test proves the check-level behavior
        # matches that: it must still PASS, not flake, when the event is
        # already sitting in the buffer before the read loop starts.
        _inject_after_fire_now(self.link, [
            Event(seq=1, sim_time_us=0, event_type=EventType.FAULT_FIRED),
            Event(seq=2, sim_time_us=1000, event_type=EventType.FAULT_FIRED),
        ])
        status, detail = st._check_event_sequence_continuity(self.link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("FAULT_FIRED", detail)

    def test_event_sequence_continuity_fail_reports_slot_state_from_fault_list(self):
        # On a genuine "no EVT frames arrived" failure, the check now queries
        # FAULT/LIST for the slot so a future flaky run can tell "never went
        # ACTIVE" (scheduling problem) apart from "went ACTIVE but no EVT
        # frame reached the PC" (transport/ring-drain problem) -- previously
        # both looked identical ("produced no EVT frames at all").
        self.link.script_response(CommandGroup.FAULT, FaultCmd.LIST, {
            "returned_count": 1,
            "faults": [{"fault_slot": 30, "state": "active", "fault_type": 0, "target": 0,
                        "fire_count": 1, "active_since_s": 0.0}],
        })
        status, detail = st._check_event_sequence_continuity(self.link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("state: active", detail)

    def test_expander_read_after_write_not_runnable_against_mock(self):
        status, detail = st._check_expander_read_after_write(self.link)
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)
        self.assertIn("MockSimLink", detail)

    def test_expander_read_after_write_skips_on_no_sample(self):
        # Bench-day case CONFIRMED BUG fix targets: a real device (not
        # virtual_simfw/MockSimLink) with no MCP23017 physically attached to
        # J20. i2c_owner_io_read()/handle_io_read() answer ERR_NO_SAMPLE
        # (cmd_ids.h 0x05) for this -- a legal request the fixture simply
        # cannot satisfy yet, not a caller mistake -- so the check must
        # SKIP with an explanatory message, never FAIL.
        self.link.script_response(CommandGroup.SYS, SysCmd.GET_VERSION, {
            "protocol_version": 1, "min_compatible": 1, "fw_version": "1.2.3",
            "fw_version_major": 1, "fw_version_minor": 2, "fw_version_patch": 3,
            "fw_git_dirty": False, "fw_git_hash": "deadbee",  # real-looking, not "virtual"/"0000000"
        })
        self.link.script_response(CommandGroup.IO, 1, {"ok": True})  # SET_DIR
        self.link.script_response(CommandGroup.IO, 2, {"ok": True})  # WRITE (high)
        self.link.script_response(CommandGroup.IO, 3, {}, error="IO/3: ERR_NO_SAMPLE")  # READ
        status, detail = st._check_expander_read_after_write(self.link)
        self.assertEqual(status, st.STATUS_SKIP)
        self.assertIn("no MCP23017 responding", detail)
        self.assertIn("attach the fixture", detail)

    def test_expander_read_after_write_still_fails_on_genuine_bad_args(self):
        # A real ERR_BAD_ARGS (the request itself is wrong) must still FAIL
        # -- only ERR_NO_SAMPLE gets the SKIP treatment. Proves the SKIP
        # branch is matching the specific status name, not swallowing every
        # SimLinkError from the READ call.
        self.link.script_response(CommandGroup.SYS, SysCmd.GET_VERSION, {
            "protocol_version": 1, "min_compatible": 1, "fw_version": "1.2.3",
            "fw_version_major": 1, "fw_version_minor": 2, "fw_version_patch": 3,
            "fw_git_dirty": False, "fw_git_hash": "deadbee",
        })
        self.link.script_response(CommandGroup.IO, 1, {"ok": True})  # SET_DIR
        self.link.script_response(CommandGroup.IO, 2, {"ok": True})  # WRITE (high)
        self.link.script_response(CommandGroup.IO, 3, {}, error="IO/3: ERR_BAD_ARGS")  # READ
        status, detail = st._check_expander_read_after_write(self.link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("ERR_BAD_ARGS", detail)

    def test_hardware_only_checks_always_not_runnable(self):
        for fn in (st._check_spi_master_loopback, st._check_ct_adc_loopback):
            status, detail = fn(self.link)
            self.assertEqual(status, st.STATUS_NOT_RUNNABLE)
            self.assertTrue(detail)


class RunSelftestTests(unittest.TestCase):
    def test_runs_every_registered_check_in_order(self):
        link = MockSimLink()
        link.connect()
        report = st.run_selftest(link)
        names = [c.name for c in report.checks]
        self.assertEqual(names, [n for n, _ in st._CHECKS])

    def test_never_raises_even_if_link_is_disconnected_mid_run(self):
        link = MockSimLink()
        link.connect()
        link.disconnect()  # every check's send_command will now raise SimLinkError
        report = st.run_selftest(link)  # must not raise
        self.assertTrue(any(c.status == st.STATUS_FAIL for c in report.checks))


if __name__ == "__main__":
    unittest.main()
