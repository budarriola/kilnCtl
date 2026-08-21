"""Unit tests for `firmware/SimFW/tools/ct_calibration/` -- the CT
calibration runner built for `firmware/SimFW/docs/PLAN.md` section 3.3's
milestone M-D (amps -> PWM-scale calibration) and
`firmware/SaftyFW/docs/CURRENT_SENSE.md` section 5's commissioning check.

Two layers:

* Pure logic (`fit.py`, `crosstalk.py`, `calibration_table.py`) is tested
  directly with synthetic numbers, no I/O, no fixture, no DUT -- these are
  the tests that would fail if the arithmetic were wrong regardless of
  hardware.
* `TestCalibrateCtAgainstVirtualSimfw` drives the real `calibrate_ct.py`
  CLI end to end against a live `virtual_simfw.exe` subprocess (same
  pattern as `test_kilnsim_virtual_simfw.py`) with `--mock-dut`, so the
  full command path (real benchproto frames to a real compiled SimFW
  simulation core) is exercised even though no real analog DUT exists to
  read back from. Skipped, not failed, if `virtual_simfw.exe` hasn't been
  built -- see that test file's own skip reasoning.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
_CT_CAL_DIR = _REPO_ROOT / "firmware" / "SimFW" / "tools" / "ct_calibration"
_CALIBRATE_CT_PY = _CT_CAL_DIR / "calibrate_ct.py"
_VIRTUAL_SIMFW_EXE = (
    _REPO_ROOT / "firmware" / "SimFW" / "tools" / "virtual_simfw" / "build" / "virtual_simfw.exe"
)

sys.path.insert(0, str(_CT_CAL_DIR))

import calibration_table as ct  # noqa: E402
import crosstalk as xt  # noqa: E402
import fit  # noqa: E402
import push_ct_cal as push  # noqa: E402
from gen_ct_cal_table import TableError  # noqa: E402


# ---------------------------------------------------------------------------
# fit.py
# ---------------------------------------------------------------------------
class FitLinearTest(unittest.TestCase):
    def test_perfect_line_recovers_gain_and_offset(self):
        commanded = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
        measured = [2.0 + 10.0 * c for c in commanded]  # gain=10, offset=2
        result = fit.fit_linear(commanded, measured)
        self.assertAlmostEqual(result.gain, 10.0, places=6)
        self.assertAlmostEqual(result.offset, 2.0, places=6)
        self.assertAlmostEqual(result.r2, 1.0, places=6)
        self.assertEqual(result.n, 6)
        self.assertAlmostEqual(result.max_abs_residual, 0.0, places=6)

    def test_noisy_line_still_recovers_gain_within_tolerance(self):
        commanded = [i / 9 for i in range(10)]
        # Small deterministic "noise" pattern, not random, so the test is
        # not flaky -- alternating +/- 0.01 around a gain=30/offset=1 line.
        measured = [1.0 + 30.0 * c + (0.01 if i % 2 == 0 else -0.01) for i, c in enumerate(commanded)]
        result = fit.fit_linear(commanded, measured)
        self.assertAlmostEqual(result.gain, 30.0, delta=0.1)
        self.assertAlmostEqual(result.offset, 1.0, delta=0.1)
        self.assertGreater(result.r2, 0.999)

    def test_too_few_points_raises(self):
        with self.assertRaises(fit.FitError):
            fit.fit_linear([0.0, 1.0], [0.0, 1.0])

    def test_mismatched_lengths_raises(self):
        with self.assertRaises(fit.FitError):
            fit.fit_linear([0.0, 0.5, 1.0, 1.5], [0.0, 0.5, 1.0])

    def test_zero_variance_x_raises(self):
        with self.assertRaises(fit.FitError):
            fit.fit_linear([0.5, 0.5, 0.5, 0.5, 0.5], [1.0, 2.0, 3.0, 1.5, 2.5])

    def test_flat_readback_reports_r2_zero_not_one(self):
        # Every measured value identical: a naive 1 - 0/0 style computation
        # could report a "perfect" r2=1.0 for a channel that never moved.
        commanded = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
        measured = [0.05] * 6
        result = fit.fit_linear(commanded, measured)
        self.assertEqual(result.r2, 0.0)
        self.assertAlmostEqual(result.gain, 0.0, places=9)


class LinearFitInvertTest(unittest.TestCase):
    def test_to_command_inverts_forward_fit(self):
        f = fit.fit_linear([0.0, 0.5, 1.0, 0.25, 0.75, 0.1],
                            [1.0, 16.0, 31.0, 8.5, 23.5, 4.0])  # gain=30, offset=1
        self.assertAlmostEqual(f.to_command(16.0), 0.5, places=4)

    def test_to_command_clamps_to_range(self):
        f = fit.fit_linear([0.0, 0.2, 0.4, 0.6, 0.8, 1.0],
                            [0.0, 6.0, 12.0, 18.0, 24.0, 30.0])  # gain=30, offset=0
        self.assertEqual(f.to_command(1000.0), 1.0)  # would be way > 1, clamped
        self.assertEqual(f.to_command(-5.0), 0.0)  # would be negative, clamped


class EvaluateFitTest(unittest.TestCase):
    def _perfect(self, gain=30.0, offset=1.0, n=8):
        commanded = [i / (n - 1) for i in range(n)]
        measured = [offset + gain * c for c in commanded]
        return fit.fit_linear(commanded, measured)

    def test_good_fit_passes(self):
        verdict = fit.evaluate_fit(self._perfect())
        self.assertTrue(verdict.ok)
        self.assertEqual(verdict.reasons, ())

    def test_low_r2_rejected(self):
        commanded = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
        # scattered, not a line
        measured = [1.0, 9.0, 2.0, 8.0, 3.0, 7.0]
        result = fit.fit_linear(commanded, measured)
        verdict = fit.evaluate_fit(result)
        self.assertFalse(verdict.ok)
        self.assertTrue(any("R²" in r for r in verdict.reasons))

    def test_negative_gain_rejected(self):
        commanded = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
        measured = [10.0 - 8.0 * c for c in commanded]  # gain = -8
        result = fit.fit_linear(commanded, measured)
        verdict = fit.evaluate_fit(result)
        self.assertFalse(verdict.ok)
        self.assertTrue(any("gain" in r for r in verdict.reasons))

    def test_flat_channel_rejected(self):
        commanded = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
        measured = [0.05, 0.051, 0.049, 0.052, 0.048, 0.050]
        result = fit.fit_linear(commanded, measured)
        verdict = fit.evaluate_fit(result)
        self.assertFalse(verdict.ok)

    def test_custom_min_r2_threshold(self):
        commanded = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
        measured = [1.0, 7.2, 12.5, 19.3, 23.8, 30.9]  # decent but not perfect
        result = fit.fit_linear(commanded, measured)
        strict = fit.evaluate_fit(result, min_r2=0.999999)
        lenient = fit.evaluate_fit(result, min_r2=0.5)
        self.assertFalse(strict.ok)
        self.assertTrue(lenient.ok)


# ---------------------------------------------------------------------------
# crosstalk.py
# ---------------------------------------------------------------------------
class CheckCrosstalkTest(unittest.TestCase):
    def test_only_driven_channel_responds_passes(self):
        result = xt.check_crosstalk(1, (0.1, 25.0, 0.05), threshold_a=2.0)
        self.assertTrue(result.passed)
        self.assertEqual(result.responded, (1,))

    def test_no_channel_responds_fails(self):
        result = xt.check_crosstalk(0, (0.1, 0.1, 0.1), threshold_a=2.0)
        self.assertFalse(result.passed)
        self.assertEqual(result.responded, ())
        self.assertIn("no channel responded", result.message)

    def test_multiple_channels_respond_fails(self):
        result = xt.check_crosstalk(0, (25.0, 22.0, 0.1), threshold_a=2.0)
        self.assertFalse(result.passed)
        self.assertEqual(result.responded, (0, 1))
        self.assertIn("crosstalk", result.message)

    def test_wrong_channel_responds_fails(self):
        result = xt.check_crosstalk(0, (0.1, 25.0, 0.1), threshold_a=2.0)
        self.assertFalse(result.passed)
        self.assertEqual(result.responded, (1,))
        self.assertIn("wrong jack", result.message)

    def test_threshold_boundary_exclusive(self):
        result = xt.check_crosstalk(0, (2.0, 0.0, 0.0), threshold_a=2.0)
        self.assertEqual(result.responded, ())
        self.assertFalse(result.passed)


class AllChannelsCleanTest(unittest.TestCase):
    def test_all_pass_is_clean(self):
        results = [
            xt.check_crosstalk(c, tuple(25.0 if i == c else 0.1 for i in range(3)), 2.0)
            for c in range(3)
        ]
        self.assertTrue(xt.all_channels_clean(results))

    def test_one_failure_is_not_clean(self):
        results = [
            xt.check_crosstalk(0, (25.0, 0.1, 0.1), 2.0),
            xt.check_crosstalk(1, (0.1, 0.1, 0.1), 2.0),  # fails: no response
            xt.check_crosstalk(2, (0.1, 0.1, 25.0), 2.0),
        ]
        self.assertFalse(xt.all_channels_clean(results))

    def test_empty_is_not_clean(self):
        self.assertFalse(xt.all_channels_clean([]))


# ---------------------------------------------------------------------------
# calibration_table.py
# ---------------------------------------------------------------------------
class CalibrationTableTest(unittest.TestCase):
    def _good_channel(self, channel: int, gain=30.0, offset=1.0) -> ct.ChannelCalibration:
        commanded = [i / 7 for i in range(8)]
        measured = [offset + gain * c for c in commanded]
        f = fit.fit_linear(commanded, measured)
        sweep = [ct.SweepPoint(c, m) for c, m in zip(commanded, measured)]
        return ct.ChannelCalibration.from_fit(channel, f, sweep)

    def test_save_refuses_without_crosstalk_pass(self):
        table = ct.CalibrationTable.new(
            crosstalk_passed=False, channels={0: self._good_channel(0)}
        )
        with self.assertRaises(ct.CalibrationTableError):
            table.save("unused/path/should/not/be/created.json")

    def test_save_refuses_with_zero_channels(self):
        table = ct.CalibrationTable.new(crosstalk_passed=True, channels={})
        with self.assertRaises(ct.CalibrationTableError):
            table.save("unused/path/should/not/be/created.json")

    def test_round_trip_save_and_load(self):
        import tempfile

        channels = {c: self._good_channel(c, gain=20.0 + 5 * c, offset=0.5 + c) for c in range(3)}
        table = ct.CalibrationTable.new(crosstalk_passed=True, channels=channels)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "cal.json"
            table.save(path)
            self.assertTrue(path.exists())
            loaded = ct.CalibrationTable.load(path)
        self.assertEqual(loaded.schema_version, ct.SCHEMA_VERSION)
        self.assertTrue(loaded.crosstalk_passed)
        self.assertEqual(set(loaded.channels.keys()), {0, 1, 2})
        for c in range(3):
            self.assertAlmostEqual(loaded.get(c).gain, channels[c].gain, places=6)
            self.assertAlmostEqual(loaded.get(c).offset, channels[c].offset, places=6)
            self.assertEqual(len(loaded.get(c).sweep), len(channels[c].sweep))

    def test_load_rejects_wrong_schema_version(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "cal.json"
            path.write_text(json.dumps({"schema_version": 999, "channels": {}}), encoding="utf-8")
            with self.assertRaises(ct.CalibrationTableError):
                ct.CalibrationTable.load(path)

    def test_to_command_uses_fit_inverse(self):
        cal = self._good_channel(0, gain=25.0, offset=2.0)
        self.assertAlmostEqual(cal.to_command(27.0), 1.0, places=4)  # (27-2)/25 = 1.0
        self.assertAlmostEqual(cal.to_command(2.0), 0.0, places=4)

    def test_to_command_clamps(self):
        cal = self._good_channel(0, gain=25.0, offset=2.0)
        self.assertEqual(cal.to_command(1000.0), 1.0)
        self.assertEqual(cal.to_command(-10.0), 0.0)

    def test_get_missing_channel_raises(self):
        table = ct.CalibrationTable.new(crosstalk_passed=True, channels={0: self._good_channel(0)})
        with self.assertRaises(ct.CalibrationTableError):
            table.get(2)


# ---------------------------------------------------------------------------
# push_ct_cal.py -- wire encode/decode, inversion reuse, and push+verify
# against a fake config_store transport (no real link, no real hardware --
# see push_ct_cal.py's module docstring for the KilnFW-side gap this cannot
# paper over).
# ---------------------------------------------------------------------------
class PushCtCalWireTest(unittest.TestCase):
    def test_encode_set_ct_cal_calibrated_round_trips_through_ct_cal_reply_shape(self):
        p = push.ChannelPush(channel=1, calibrated=True, gain=0.0345, offset=-1.2)
        frame = push.encode_set_ct_cal(p)
        self.assertEqual(len(frame), push.SET_CT_CAL_LEN)
        self.assertEqual(frame[0], push.SET_CT_CAL_CMD)
        self.assertEqual(frame[1], 1)  # channel
        self.assertEqual(frame[2], 1)  # calibrated

    def test_encode_set_ct_cal_uncalibrated_forces_zero_gain_offset(self):
        # Even if a caller built a ChannelPush with stale numbers next to
        # calibrated=False, the encoded frame must carry 0.0/0.0 -- never a
        # fabricated "identity" calibration on the wire.
        p = push.ChannelPush(channel=0, calibrated=False, gain=99.0, offset=99.0)
        frame = push.encode_set_ct_cal(p)
        decoded_cal, decoded_gain, decoded_offset = struct_unpack_channel(frame[2:])
        self.assertEqual(decoded_cal, 0)
        self.assertEqual(decoded_gain, 0.0)
        self.assertEqual(decoded_offset, 0.0)

    def test_encode_set_ct_cal_rejects_out_of_range_channel(self):
        with self.assertRaises(push.CtCalWireError):
            push.encode_set_ct_cal(push.ChannelPush(channel=3, calibrated=True, gain=1.0, offset=0.0))

    def test_encode_get_ct_cal_is_one_byte(self):
        self.assertEqual(push.encode_get_ct_cal(), bytes([push.GET_CT_CAL_CMD]))

    def test_decode_ct_cal_reply_round_trips_all_channels(self):
        pushes = (
            push.ChannelPush(0, True, 28.4, 0.12),
            push.ChannelPush(1, False, 0.0, 0.0),
            push.ChannelPush(2, True, 30.9, -0.05),
        )
        payload = _encode_fake_reply(pushes)
        decoded = push.decode_ct_cal_reply(payload)
        self.assertEqual(len(decoded), 3)
        for sent, got in zip(pushes, decoded):
            self.assertEqual(sent.channel, got.channel)
            self.assertEqual(sent.calibrated, got.calibrated)
            if sent.calibrated:
                self.assertAlmostEqual(sent.gain, got.gain, places=4)
                self.assertAlmostEqual(sent.offset, got.offset, places=4)

    def test_decode_ct_cal_reply_rejects_wrong_length(self):
        with self.assertRaises(push.CtCalWireError):
            push.decode_ct_cal_reply(bytes([push.CT_CAL_REPLY_CMD, 0, 0]))

    def test_decode_ct_cal_reply_rejects_wrong_cmd_byte(self):
        good = _encode_fake_reply((push.ChannelPush(0, False, 0.0, 0.0),) * 3)
        bad = bytes([0x02]) + good[1:]
        with self.assertRaises(push.CtCalWireError):
            push.decode_ct_cal_reply(bad)


def struct_unpack_channel(chunk: bytes):
    import struct as _struct

    return _struct.unpack("<Bff", chunk)


def _encode_fake_reply(channels) -> bytes:
    out = bytearray([push.CT_CAL_REPLY_CMD])
    for c in channels:
        out += push._CT_CAL_CHANNEL_STRUCT.pack(1 if c.calibrated else 0, c.gain, c.offset)
    return bytes(out)


class BuildPushesFromJsonTest(unittest.TestCase):
    """Exercises the inversion reuse (gen_ct_cal_table.load_channels) and the
    "uncalibrated channel stays explicitly uncalibrated" requirement."""

    def _write(self, tmp_dir: Path, raw: dict) -> Path:
        p = Path(tmp_dir) / "cal.json"
        p.write_text(json.dumps(raw), encoding="utf-8")
        return p

    def test_inverts_fit_exactly_like_gen_ct_cal_table(self):
        import tempfile

        # measured_a = 25 * commanded + 2  =>  gain = 1/25, offset = -2/25
        raw = {
            "schema_version": 1,
            "crosstalk_passed": True,
            "channels": {"0": {"gain": 25.0, "offset": 2.0, "r2": 0.999, "n_points": 10}},
        }
        with tempfile.TemporaryDirectory() as d:
            path = self._write(d, raw)
            pushes = push.build_pushes_from_json(path)
        self.assertEqual(len(pushes), 3)
        ch0 = pushes[0]
        self.assertTrue(ch0.calibrated)
        self.assertAlmostEqual(ch0.gain, 1.0 / 25.0, places=9)
        self.assertAlmostEqual(ch0.offset, -2.0 / 25.0, places=9)

    def test_missing_channel_is_explicit_uncalibrated_not_identity(self):
        import tempfile

        raw = {
            "schema_version": 1,
            "crosstalk_passed": True,
            "channels": {
                "0": {"gain": 25.0, "offset": 2.0, "r2": 0.999, "n_points": 10},
                "2": {"gain": 30.0, "offset": -1.0, "r2": 0.995, "n_points": 10},
            },
        }
        with tempfile.TemporaryDirectory() as d:
            path = self._write(d, raw)
            pushes = push.build_pushes_from_json(path)
        by_channel = {p.channel: p for p in pushes}
        self.assertTrue(by_channel[0].calibrated)
        self.assertTrue(by_channel[2].calibrated)
        # Channel 1 was never in the table -- must be explicit uncalibrated,
        # not a silently-fabricated gain=1/offset=0 "identity" pass-through.
        self.assertFalse(by_channel[1].calibrated)
        self.assertEqual(by_channel[1].gain, 0.0)
        self.assertEqual(by_channel[1].offset, 0.0)

    def test_refuses_table_that_failed_crosstalk(self):
        import tempfile

        raw = {
            "schema_version": 1,
            "crosstalk_passed": False,
            "channels": {"0": {"gain": 25.0, "offset": 2.0, "r2": 0.999, "n_points": 10}},
        }
        with tempfile.TemporaryDirectory() as d:
            path = self._write(d, raw)
            with self.assertRaises(TableError):
                push.build_pushes_from_json(path)

    def test_refuses_non_invertible_gain(self):
        import tempfile

        raw = {
            "schema_version": 1,
            "crosstalk_passed": True,
            "channels": {"0": {"gain": 0.0, "offset": 2.0, "r2": 0.999, "n_points": 10}},
        }
        with tempfile.TemporaryDirectory() as d:
            path = self._write(d, raw)
            with self.assertRaises(TableError):
                push.build_pushes_from_json(path)


class _FakeConfigStoreTransport:
    """In-memory stand-in for SaftyFW's config_store.c CT_CAL read-modify-write
    semantics (config_store.h's `config_store_get_ct_cal()` /
    `link_task_handle_set_ct_cal()`) -- NOT a hardware model and NOT a claim
    about real ESP/Pico firmware behavior (see push_ct_cal.py's module
    docstring: the real ESP does not relay these commands at all today).
    Just enough of the documented wire behavior -- start every channel
    uncalibrated, and each SET_CT_CAL overwrites exactly one channel without
    disturbing the others -- to exercise push_and_verify() end to end
    without any real link."""

    def __init__(self, num_channels: int = 3) -> None:
        self._channels = [push.ChannelPush(c, False, 0.0, 0.0) for c in range(num_channels)]
        self.sent_frames: list[bytes] = []

    def send_set_ct_cal(self, payload: bytes) -> None:
        self.sent_frames.append(payload)
        _cmd, channel, calibrated, gain, offset = push._SET_CT_CAL_STRUCT.unpack(payload)
        self._channels[channel] = push.ChannelPush(channel, bool(calibrated), gain, offset)

    def query_ct_cal(self, payload: bytes, timeout: float) -> bytes:
        assert payload == push.encode_get_ct_cal()
        return _encode_fake_reply(tuple(self._channels))

    def close(self) -> None:
        pass


class PushAndVerifyTest(unittest.TestCase):
    def test_calibrated_channels_push_and_verify_clean(self):
        transport = _FakeConfigStoreTransport()
        pushes = (
            push.ChannelPush(0, True, 0.04, -0.08),
            push.ChannelPush(1, True, 0.0317, 0.02),
            push.ChannelPush(2, False, 0.0, 0.0),
        )
        results = push.push_and_verify(transport, pushes, log=lambda line="": None)
        self.assertTrue(all(r.matches for r in results))
        self.assertEqual(len(transport.sent_frames), 3)

    def test_uncalibrated_channel_lands_as_uncalibrated_not_identity(self):
        transport = _FakeConfigStoreTransport()
        # Only channel 0 calibrated -- channels 1 and 2 explicit uncalibrated,
        # matching what build_pushes_from_json() would produce for a table
        # missing those channels.
        pushes = (
            push.ChannelPush(0, True, 0.04, -0.08),
            push.ChannelPush(1, False, 0.0, 0.0),
            push.ChannelPush(2, False, 0.0, 0.0),
        )
        push.push_and_verify(transport, pushes, log=lambda line="": None)
        self.assertFalse(transport._channels[1].calibrated)
        self.assertFalse(transport._channels[2].calibrated)
        # And pushing channel 0 must never have disturbed 1/2's flags, which
        # were already uncalibrated by construction here, so also check the
        # read-modify-write claim the other direction: pushing 1 leaves 0 alone.
        transport2 = _FakeConfigStoreTransport()
        push.push_and_verify(
            transport2,
            (push.ChannelPush(0, True, 10.0, 1.0),),
            log=lambda line="": None,
        )
        self.assertFalse(transport2._channels[1].calibrated)
        self.assertFalse(transport2._channels[2].calibrated)
        self.assertTrue(transport2._channels[0].calibrated)

    def test_verify_catches_a_readback_mismatch(self):
        class _LyingTransport(_FakeConfigStoreTransport):
            def query_ct_cal(self, payload: bytes, timeout: float) -> bytes:
                # Simulate a firmware bug: channel 0 comes back with the
                # wrong gain even though what was sent (and stored, per the
                # base class) was correct.
                corrupted = list(self._channels)
                corrupted[0] = push.ChannelPush(0, True, corrupted[0].gain + 5.0, corrupted[0].offset)
                return _encode_fake_reply(tuple(corrupted))

        transport = _LyingTransport()
        pushes = (push.ChannelPush(0, True, 0.04, -0.08),)
        with self.assertRaises(push.VerifyMismatchError) as ctx:
            push.push_and_verify(transport, pushes, log=lambda line="": None)
        self.assertIn("channel 0", str(ctx.exception))

    def test_verify_catches_a_calibrated_flag_mismatch(self):
        class _FlagFlipTransport(_FakeConfigStoreTransport):
            def query_ct_cal(self, payload: bytes, timeout: float) -> bytes:
                corrupted = list(self._channels)
                corrupted[0] = push.ChannelPush(0, False, 0.0, 0.0)  # firmware "forgot" it
                return _encode_fake_reply(tuple(corrupted))

        transport = _FlagFlipTransport()
        pushes = (push.ChannelPush(0, True, 0.04, -0.08),)
        with self.assertRaises(push.VerifyMismatchError):
            push.push_and_verify(transport, pushes, log=lambda line="": None)


# ---------------------------------------------------------------------------
# End-to-end against virtual_simfw (real protocol, synthetic DUT)
# ---------------------------------------------------------------------------
@unittest.skipUnless(
    _VIRTUAL_SIMFW_EXE.exists(),
    f"virtual_simfw.exe not built -- run build_host.ps1 in {_VIRTUAL_SIMFW_EXE.parent.parent}",
)
class TestCalibrateCtAgainstVirtualSimfw(unittest.TestCase):
    """Drives the real `calibrate_ct.py` CLI against a live virtual_simfw
    subprocess with `--mock-dut`. This proves the procedure, the wire
    round-trip, the fit math, the crosstalk gate, and the output format --
    it does NOT prove anything about real analog current-sense hardware
    (see readback.py's module docstring)."""

    def setUp(self):
        self.proc = subprocess.Popen(
            [str(_VIRTUAL_SIMFW_EXE), "--port", "0", "--seed", "7"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        line = self.proc.stdout.readline()
        if "VIRTUAL_SIMFW_LISTENING" not in line:
            self.proc.kill()
            self.fail(f"virtual_simfw did not report listening: {line!r}")
        self.port = int(line.strip().split("port=")[1].split()[0])

    def tearDown(self):
        self.proc.kill()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass

    def _run(self, extra_args, out_path, report_path=None):
        args = [
            sys.executable, str(_CALIBRATE_CT_PY),
            "--virtual", f"127.0.0.1:{self.port}",
            "--mock-dut", "--points", "5", "--settle-s", "0.08",
            "--samples-per-point", "1",
            "--out", str(out_path),
        ]
        if report_path:
            args += ["--report-out", str(report_path)]
        args += extra_args
        return subprocess.run(args, capture_output=True, text=True, timeout=60)

    def test_clean_run_writes_valid_calibration_table(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "cal.json"
            result = self._run(["--mock-dut-leak", "0"], out)
            self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
            self.assertTrue(out.exists())
            table = ct.CalibrationTable.load(out)
            self.assertTrue(table.crosstalk_passed)
            self.assertEqual(set(table.channels.keys()), {0, 1, 2})
            for c in range(3):
                cal = table.get(c)
                self.assertGreater(cal.gain, 0.0)
                self.assertGreaterEqual(cal.r2, 0.98)

    def test_crosstalk_leak_is_detected_and_blocks_output(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "cal.json"
            result = self._run(["--mock-dut-leak", "0.15"], out)
            self.assertEqual(result.returncode, 1, msg=result.stdout + result.stderr)
            self.assertFalse(out.exists())
            self.assertIn("CROSSTALK CHECK FAILED", result.stdout)

    def test_flat_channel_rejected_after_skipping_crosstalk(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "cal.json"
            result = self._run(
                ["--mock-dut-leak", "0", "--mock-dut-flat-channel", "1", "--skip-crosstalk"], out
            )
            self.assertEqual(result.returncode, 2, msg=result.stdout + result.stderr)
            self.assertFalse(out.exists())
            self.assertIn("FITS REJECTED", result.stdout)


if __name__ == "__main__":
    unittest.main()
