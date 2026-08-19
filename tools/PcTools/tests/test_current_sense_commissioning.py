#!/usr/bin/env python3
"""Unit tests for kilnctrl-adjacent script current_sense_commissioning.py --
pure decision logic only (step 1 "zero", step 2 "exactly one channel
responds", step 4 "decay"), against SYNTHETIC readings. No serial port, no
UART link, and no real SaftyFW/RP2040 hardware is used or required.

These tests do NOT exercise the bench I/O path (run_channel/main, which talk
to the shared UART link) -- that path is unexercised against real hardware
entirely, per the script's own module docstring, because no MAX31856/CT
wiring exists yet. What is tested here is only that the pass/fail arithmetic
matches CURRENT_SENSE.md Sec.5's stated rules.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))

import current_sense_commissioning as csc  # noqa: E402


class CheckZeroTest(unittest.TestCase):
    def test_all_small_passes(self):
        result = csc.check_zero((0.05, 0.02, 0.0), max_expected_a=2.0)
        self.assertTrue(result.passed)

    def test_one_channel_above_threshold_fails(self):
        result = csc.check_zero((0.05, 2.5, 0.0), max_expected_a=2.0)
        self.assertFalse(result.passed)
        self.assertIn("1", result.message)

    def test_multiple_channels_above_threshold_reported(self):
        result = csc.check_zero((3.0, 0.0, 4.0), max_expected_a=2.0)
        self.assertFalse(result.passed)
        self.assertIn("0", result.message)
        self.assertIn("2", result.message)


class CheckSingleResponseTest(unittest.TestCase):
    def test_exactly_expected_channel_passes(self):
        result = csc.check_single_response((0.1, 8.0, 0.1), expected_channel=1, threshold_a=2.0)
        self.assertTrue(result.passed)
        self.assertEqual(result.responded, [1])

    def test_no_channel_responds_fails(self):
        result = csc.check_single_response((0.1, 0.1, 0.1), expected_channel=1, threshold_a=2.0)
        self.assertFalse(result.passed)
        self.assertEqual(result.responded, [])
        self.assertIn("no channel responded", result.message)

    def test_multiple_channels_respond_fails(self):
        result = csc.check_single_response((0.1, 8.0, 7.5), expected_channel=1, threshold_a=2.0)
        self.assertFalse(result.passed)
        self.assertEqual(result.responded, [1, 2])
        self.assertIn("all responded", result.message)

    def test_wrong_channel_responds_fails_with_ct_hint(self):
        result = csc.check_single_response((8.0, 0.1, 0.1), expected_channel=1, threshold_a=2.0)
        self.assertFalse(result.passed)
        self.assertEqual(result.responded, [0])
        self.assertIn("wrong jack", result.message)

    def test_threshold_boundary_is_exclusive(self):
        # exactly at threshold does not count as "responded"
        result = csc.check_single_response((2.0, 0.0, 0.0), expected_channel=0, threshold_a=2.0)
        self.assertEqual(result.responded, [])
        self.assertFalse(result.passed)


class DecayPctRemainingTest(unittest.TestCase):
    def test_full_decay_to_zero(self):
        self.assertEqual(csc.decay_pct_remaining(10.0, 0.0), 0.0)

    def test_half_remaining(self):
        self.assertAlmostEqual(csc.decay_pct_remaining(10.0, 5.0), 50.0)

    def test_nonpositive_baseline_is_zero(self):
        self.assertEqual(csc.decay_pct_remaining(0.0, 5.0), 0.0)
        self.assertEqual(csc.decay_pct_remaining(-1.0, 5.0), 0.0)

    def test_negative_sample_clamped(self):
        self.assertEqual(csc.decay_pct_remaining(10.0, -3.0), 0.0)


class CheckDecayTest(unittest.TestCase):
    def test_fast_decay_within_deadline_passes(self):
        samples = [
            csc.DecaySample(0.5, 6.0),
            csc.DecaySample(1.0, 2.0),
            csc.DecaySample(1.5, 0.2),  # 2% of 10A baseline
            csc.DecaySample(2.0, 0.1),
        ]
        result = csc.check_decay(10.0, samples, threshold_pct=5.0, deadline_s=4.0)
        self.assertTrue(result.passed)
        self.assertEqual(result.elapsed_s, 1.5)

    def test_slow_decay_past_deadline_fails(self):
        samples = [
            csc.DecaySample(1.0, 8.0),
            csc.DecaySample(2.0, 6.0),
            csc.DecaySample(3.0, 4.0),
            csc.DecaySample(4.0, 3.0),  # 30% remaining at deadline -- fails
        ]
        result = csc.check_decay(10.0, samples, threshold_pct=5.0, deadline_s=4.0)
        self.assertFalse(result.passed)
        self.assertIn("C57 or R77", result.message)

    def test_meets_threshold_but_past_deadline_fails(self):
        samples = [
            csc.DecaySample(4.5, 0.1),  # would pass the % but arrives too late
        ]
        result = csc.check_decay(10.0, samples, threshold_pct=5.0, deadline_s=4.0)
        self.assertFalse(result.passed)

    def test_no_samples_fails(self):
        result = csc.check_decay(10.0, [], threshold_pct=5.0, deadline_s=4.0)
        self.assertFalse(result.passed)
        self.assertIn("no decay samples", result.message)

    def test_nonpositive_baseline_fails_explicitly(self):
        result = csc.check_decay(0.0, [csc.DecaySample(1.0, 0.0)], threshold_pct=5.0, deadline_s=4.0)
        self.assertFalse(result.passed)
        self.assertIn("not positive", result.message)


class ChannelResultPassedTest(unittest.TestCase):
    def _passing_channel(self, channel=0, relay=1):
        return csc.ChannelResult(
            channel=channel,
            relay=relay,
            zero=csc.ZeroResult([0.05, 0.02, 0.0], True, "ok"),
            step2=csc.Step2Result([channel], channel, True, "ok"),
            decay=csc.DecayResult(True, 2.0, 1.0, "ok"),
        )

    def test_all_automated_steps_pass(self):
        self.assertTrue(self._passing_channel().passed)

    def test_step3_manual_fields_do_not_gate_pass(self):
        ch = self._passing_channel()
        ch.manual_amps = None
        ch.manual_delta_a = None
        self.assertTrue(ch.passed)  # step 3 is cosmetic per CURRENT_SENSE.md

    def test_missing_zero_fails_overall(self):
        ch = self._passing_channel()
        ch.zero = None
        self.assertFalse(ch.passed)

    def test_failing_step2_fails_overall_even_if_decay_passes(self):
        ch = self._passing_channel()
        ch.step2 = csc.Step2Result([1], 0, False, "wrong channel")
        self.assertFalse(ch.passed)

    def test_failing_decay_fails_overall(self):
        ch = self._passing_channel()
        ch.decay = csc.DecayResult(False, 20.0, 4.0, "too slow")
        self.assertFalse(ch.passed)


class S3S4SafeTest(unittest.TestCase):
    def _channel(self, channel, step2_pass):
        return csc.ChannelResult(
            channel=channel,
            relay=channel + 1,
            step2=csc.Step2Result([channel] if step2_pass else [], channel, step2_pass, "x"),
        )

    def test_all_three_pass_step2_is_safe(self):
        results = [self._channel(0, True), self._channel(1, True), self._channel(2, True)]
        self.assertTrue(csc.s3_s4_safe(results))

    def test_one_channel_fails_step2_is_unsafe(self):
        results = [self._channel(0, True), self._channel(1, False), self._channel(2, True)]
        self.assertFalse(csc.s3_s4_safe(results))

    def test_fewer_than_three_channels_is_unsafe(self):
        results = [self._channel(0, True), self._channel(1, True)]
        self.assertFalse(csc.s3_s4_safe(results))

    def test_missing_step2_result_is_unsafe(self):
        ch = csc.ChannelResult(channel=0, relay=1, step2=None)
        results = [ch, self._channel(1, True), self._channel(2, True)]
        self.assertFalse(csc.s3_s4_safe(results))


class ParseRelayMapTest(unittest.TestCase):
    def test_default_style_string(self):
        self.assertEqual(csc.parse_relay_map("1:0,2:1,3:2"), {1: 0, 2: 1, 3: 2})

    def test_whitespace_tolerant(self):
        self.assertEqual(csc.parse_relay_map(" 1:0 , 2:1 "), {1: 0, 2: 1})

    def test_empty_segments_ignored(self):
        self.assertEqual(csc.parse_relay_map("1:0,,2:1,"), {1: 0, 2: 1})


class ArgParserTest(unittest.TestCase):
    def test_defaults(self):
        args = csc.build_arg_parser().parse_args([])
        self.assertEqual(args.relay_map, "1:0,2:1,3:2")
        self.assertEqual(args.threshold_a, csc.DEFAULT_THRESHOLD_A)
        self.assertFalse(args.non_interactive)

    def test_overrides_parse(self):
        args = csc.build_arg_parser().parse_args([
            "--relay-map", "1:2,2:0,3:1",
            "--threshold-a", "1.5",
            "--decay-pct", "10",
            "--decay-deadline-s", "6",
            "--settle-s", "0.2",
            "--non-interactive",
        ])
        self.assertEqual(args.relay_map, "1:2,2:0,3:1")
        self.assertEqual(args.threshold_a, 1.5)
        self.assertEqual(args.decay_pct, 10.0)
        self.assertEqual(args.decay_deadline_s, 6.0)
        self.assertEqual(args.settle_s, 0.2)
        self.assertTrue(args.non_interactive)


if __name__ == "__main__":
    unittest.main()
