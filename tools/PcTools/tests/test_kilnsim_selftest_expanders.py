#!/usr/bin/env python3
"""Tests for the expander-coverage checks added to kilnsim.selftest on top of
the pre-existing ``expander_read_after_write`` (see test_kilnsim_selftest.py
for that one and for the CheckResult/SelftestReport/_run_check machinery
these checks share). Deliberately a SEPARATE file, per this pass's own
instructions, rather than editing test_kilnsim_selftest.py or
test_kilnsim_cli.py (both owned by other concurrent work).

Every check under test here is (link) -> (status, detail), run directly
against :class:`~kilnsim.link.MockSimLink` -- no hardware, matching every
other kilnsim test module's convention (see that class's own docstring for
why nothing in this package should need real hardware to be testable).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import selftest as st  # noqa: E402
from kilnsim.link import MockSimLink  # noqa: E402
from kilnsim.protocol import CommandGroup, SysCmd  # noqa: E402


#: A "real-looking" fw_git_hash -- neither virtual_simfw's "virtual" nor
#: MockSimLink's own canned "0000000" default -- so _classify_expander_link()
#: treats this mock as if it were talking to real SimFW hardware and lets a
#: check actually attempt its SET_DIR/WRITE/READ sequence instead of bailing
#: out NOT_RUNNABLE immediately. Mirrors test_kilnsim_selftest.py's own
#: expander_read_after_write tests, which use the identical trick.
_REAL_LOOKING_VERSION = {
    "protocol_version": 1, "min_compatible": 1, "fw_version": "1.2.3",
    "fw_version_major": 1, "fw_version_minor": 2, "fw_version_patch": 3,
    "fw_git_dirty": False, "fw_git_hash": "deadbee",
}


def _make_real_looking_link() -> MockSimLink:
    link = MockSimLink()
    link.connect()
    link.script_response(CommandGroup.SYS, SysCmd.GET_VERSION, _REAL_LOOKING_VERSION)
    return link


class ClassifyExpanderLinkTests(unittest.TestCase):
    """The shared NOT_RUNNABLE-detection helper every check below reuses --
    tested once here directly rather than once per check."""

    def test_mock_default_is_not_runnable(self):
        link = MockSimLink()
        link.connect()
        verdict = st._classify_expander_link(link)
        self.assertIsNotNone(verdict)
        status, detail = verdict
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)
        self.assertIn("MockSimLink", detail)

    def test_real_looking_hash_is_none(self):
        link = _make_real_looking_link()
        self.assertIsNone(st._classify_expander_link(link))


class ExpanderExp2ReadAfterWriteTests(unittest.TestCase):
    """exp2 (0x26, protocol exp=1) round trip -- previously zero coverage."""

    def test_not_runnable_against_mock_default(self):
        link = MockSimLink()
        link.connect()
        status, detail = st._check_expander_exp2_read_after_write(link)
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)

    def test_passes_and_uses_exp1_flag_against_real_looking_link(self):
        # MockSimLink's IO/READ default always answers {"level": False} --
        # scripting IO/3 (READ) to answer True first (the "wrote high"
        # poll) then False (the "wrote low" poll) simulates a genuinely
        # working expander without needing real hardware.
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 3, {"level": True})   # READ after WRITE(high)
        link.script_response(CommandGroup.IO, 3, {"level": False})  # READ after WRITE(low)
        status, detail = st._check_expander_exp2_read_after_write(link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("exp1 (0x26)", detail)
        # SET_DIR/WRITE/READ commands, plus the restore-to-default SET_DIR
        # in the finally block, must all have gone to exp=1 (I2C_OWNER_EXP_2).
        io_sent = [(cmd, payload) for group, cmd, payload in link.sent_commands if group == CommandGroup.IO]
        self.assertTrue(all(p.get("exp") == 1 for _, p in io_sent), io_sent)

    def test_skips_on_err_no_sample(self):
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 2, {}, error="IO/2: ERR_NO_SAMPLE")  # WRITE(high) fails
        status, detail = st._check_expander_exp2_read_after_write(link)
        self.assertEqual(status, st.STATUS_SKIP)
        self.assertIn("0x26", detail)

    # --- NEGATIVE TEST: prove this check can actually fail -----------------
    def test_fails_when_expander_never_reports_the_written_level(self):
        # A "broken" expander that always reads back False no matter what
        # was written -- the check must FAIL, not time out silently or
        # report a false PASS. (IO/3 left unscripted so every READ keeps
        # answering MockSimLink's default {"level": False}; the "wrote
        # high" poll then never sees True and _expander_read_until()
        # returns its last-seen (still False) reading once its 0.5s
        # timeout elapses.)
        link = _make_real_looking_link()
        status, detail = st._check_expander_exp2_read_after_write(link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("False", detail)


class ExpanderExp1MultiPinTests(unittest.TestCase):
    def test_not_runnable_against_mock_default(self):
        link = MockSimLink()
        link.connect()
        status, _ = st._check_expander_exp1_multi_pin_read_after_write(link)
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)

    def test_passes_when_every_pin_round_trips(self):
        link = _make_real_looking_link()
        # 3 pins (9, 11, 15), each does WRITE(high)->READ, WRITE(low)->READ:
        # queue two scripted READ replies per pin, in pin order.
        for _ in range(3):
            link.script_response(CommandGroup.IO, 3, {"level": True})
            link.script_response(CommandGroup.IO, 3, {"level": False})
        status, detail = st._check_expander_exp1_multi_pin_read_after_write(link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("[9, 11, 15]", detail)

    def test_never_touches_a_reserved_pin(self):
        link = _make_real_looking_link()
        for _ in range(3):
            link.script_response(CommandGroup.IO, 3, {"level": True})
            link.script_response(CommandGroup.IO, 3, {"level": False})
        st._check_expander_exp1_multi_pin_read_after_write(link)
        io_sent = [payload for group, cmd, payload in link.sent_commands if group == CommandGroup.IO]
        touched_pins = {p.get("pin") for p in io_sent}
        self.assertTrue(touched_pins.isdisjoint(st.EXP1_RESERVED_PINS), touched_pins)

    # --- NEGATIVE TEST -------------------------------------------------
    def test_fails_and_names_the_offending_pin_when_one_pin_misbehaves(self):
        # Pin 9 round-trips fine; pins 11 and 15 are left unscripted, so
        # every IO/READ for them falls through to MockSimLink's default
        # {"level": False} and their "wrote high" poll never sees True --
        # broken-pin behavior. (Not scripting exactly one bad pin: MockSimLink
        # queues replies FIFO per (group, cmd) with no way to key a reply to
        # a specific pin, so an unscripted call is the only way to make a
        # SPECIFIC pin's poll fail without also silently reassigning a later
        # pin's intended scripted reply to an earlier pin's poll.) Must FAIL
        # and the detail must name pin 11 specifically, not just "something
        # failed".
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 3, {"level": True})   # pin 9 high
        link.script_response(CommandGroup.IO, 3, {"level": False})  # pin 9 low
        status, detail = st._check_expander_exp1_multi_pin_read_after_write(link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("11", detail)
        self.assertNotIn("9:", detail)  # pin 9 must not be reported as one of the bad pins


class ExpanderPinIndependenceTests(unittest.TestCase):
    def test_not_runnable_against_mock_default(self):
        link = MockSimLink()
        link.connect()
        status, _ = st._check_expander_pin_independence(link)
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)

    def test_passes_when_pins_are_independent(self):
        link = _make_real_looking_link()
        # Sequence of IO/READ (cmd 3) calls the check makes, in order:
        #   1. poll for pin_a low  (WRITE(a, False) confirm)
        #   2. poll for pin_b low  (WRITE(b, False) confirm)
        #   3. poll for pin_a high (WRITE(a, True) confirm)
        #   4. immediate READ pin_b -> must still read False to prove independence
        #   5. poll for pin_b high (WRITE(b, True) confirm)
        #   6. immediate READ pin_a -> must still read True to prove independence
        for level in (False, False, True, False, True, True):
            link.script_response(CommandGroup.IO, 3, {"level": level})
        status, detail = st._check_expander_pin_independence(link)
        self.assertEqual(status, st.STATUS_PASS)

    # --- NEGATIVE TEST -------------------------------------------------
    def test_fails_when_pin_b_is_disturbed_by_writing_pin_a(self):
        # Simulates exactly the shadow-register bug this check exists to
        # catch: writing pin_a high also flips pin_b's reported level to
        # True (step 4 below should have stayed False).
        link = _make_real_looking_link()
        levels = [False, False, True, True]  # step 4 wrongly reads True
        for level in levels:
            link.script_response(CommandGroup.IO, 3, {"level": level})
        status, detail = st._check_expander_pin_independence(link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("shadow-register", detail)


class ExpanderInputPullupTests(unittest.TestCase):
    def test_not_runnable_against_mock_default(self):
        link = MockSimLink()
        link.connect()
        status, _ = st._check_expander_input_pullup_reads_high(link)
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)

    def test_passes_when_both_pins_read_high(self):
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 3, {"level": True})  # exp1 pin 11
        link.script_response(CommandGroup.IO, 3, {"level": True})  # exp2 pin 5
        status, detail = st._check_expander_input_pullup_reads_high(link)
        self.assertEqual(status, st.STATUS_PASS)

    # --- NEGATIVE TEST -------------------------------------------------
    def test_fails_when_a_pin_reads_low_with_pullup_enabled(self):
        # Simulates something externally pulling one pin low (or a dead
        # pull-up) -- MockSimLink's unscripted IO/READ default is already
        # {"level": False}, so leaving the second probe's reply unscripted
        # is enough to reproduce this.
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 3, {"level": True})  # exp1 pin 11: fine
        # exp2 pin 5's READ falls through to MockSimLink's default False.
        status, detail = st._check_expander_input_pullup_reads_high(link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("pull-up", detail)


class ExpanderReservedPinRejectedTests(unittest.TestCase):
    def test_not_runnable_against_mock_default(self):
        link = MockSimLink()
        link.connect()
        status, _ = st._check_expander_reserved_pin_rejected(link)
        self.assertEqual(status, st.STATUS_NOT_RUNNABLE)

    def test_passes_when_firmware_rejects_with_err_bad_args(self):
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 1, {}, error="IO/1: ERR_BAD_ARGS")  # SET_DIR
        status, detail = st._check_expander_reserved_pin_rejected(link)
        self.assertEqual(status, st.STATUS_PASS)
        self.assertIn("ERR_BAD_ARGS", detail)

    def test_passes_when_firmware_rejects_with_err_busy(self):
        # Tolerance for the concurrently-in-flight ERR_BUSY change this
        # module's task description calls out -- still a rejection, must
        # still PASS.
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 1, {}, error="IO/1: ERR_BUSY")
        status, detail = st._check_expander_reserved_pin_rejected(link)
        self.assertEqual(status, st.STATUS_PASS)

    def test_restores_pin_to_input_pullup_even_when_rejected(self):
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 1, {}, error="IO/1: ERR_BAD_ARGS")
        st._check_expander_reserved_pin_rejected(link)
        # Last IO command sent must be the finally-block restore: SET_DIR,
        # input=True, pullup=True, on the same (exp, pin).
        last_group, last_cmd, last_payload = link.sent_commands[-1]
        self.assertEqual((last_group, last_cmd), (CommandGroup.IO, 1))
        self.assertEqual(last_payload.get("exp"), 0)
        self.assertEqual(last_payload.get("pin"), 0)
        self.assertTrue(last_payload.get("is_input"))
        self.assertTrue(last_payload.get("pullup"))

    # --- NEGATIVE TEST: prove this check can actually fail -----------------
    def test_fails_when_firmware_wrongly_accepts_the_reserved_pin(self):
        # The interlock-failure case this check exists to catch: SET_DIR on
        # a reserved pin succeeds instead of being rejected.
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 1, {})  # SET_DIR "succeeds" -- no error
        status, detail = st._check_expander_reserved_pin_rejected(link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("NOT rejected", detail)

    def test_fails_on_unrecognized_rejection_status(self):
        # Rejected, but with a status this check doesn't recognize as a
        # reserved-pin rejection -- must not be silently treated as PASS.
        link = _make_real_looking_link()
        link.script_response(CommandGroup.IO, 1, {}, error="IO/1: ERR_NO_SAMPLE")
        status, detail = st._check_expander_reserved_pin_rejected(link)
        self.assertEqual(status, st.STATUS_FAIL)
        self.assertIn("not with a recognized rejection status", detail)


class RunSelftestIncludesNewChecksTests(unittest.TestCase):
    def test_all_five_new_checks_are_registered(self):
        names = [n for n, _ in st._CHECKS]
        for expected in (
            "expander_exp2_read_after_write",
            "expander_exp1_multi_pin_read_after_write",
            "expander_pin_independence",
            "expander_input_pullup_reads_high",
            "expander_reserved_pin_rejected",
        ):
            self.assertIn(expected, names)

    def test_run_selftest_never_raises_with_new_checks_included(self):
        link = MockSimLink()
        link.connect()
        report = st.run_selftest(link)  # must not raise
        self.assertTrue(len(report.checks) >= len(st._CHECKS))


if __name__ == "__main__":
    unittest.main()
