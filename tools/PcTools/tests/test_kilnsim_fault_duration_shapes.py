#!/usr/bin/env python3
"""Regression tests for the FAULT_SCHEDULE `duration`/`repeat` shape bug:

`_encode_fault_schedule()` (kilnsim/payloads.py) required `payload["duration"]`
to be a dict ({"kind": ..., ...}), but `kilnsim.cli`'s `fault` subcommand
passed a bare string ("permanent") and `kilnsim.mcp_server`'s `fault_schedule`
tool did the same for a missing `duration`/`repeat` ("permanent"/"once").
Every single `kilnsim fault ...` CLI invocation crashed with:

    AttributeError: 'str' object has no attribute 'get'

Nothing caught this because the only exerciser of the encoder,
kilnsim/selftest.py, always used the dict form -- and there was no CLI-path
coverage at all (`grep -rn "cmd_fault" tools/PcTools/tests/` returned nothing
before this file).

The fix normalizes `duration`/`repeat` in `_encode_fault_schedule()` itself
(dict / bare-string-kind / int-kind / None-as-default, all accepted) and
also fixes the two broken callers (cli.py, mcp_server.py) to pass the
documented dict form, plus kilnsim.link's MockSimLink (`_default_response`)
which had the identical `.get("kind")` assumption on the mock path used by
`--mock` testing.

Run with: python -m unittest discover -s tools/PcTools/tests
(pytest also collects this file directly, which is how it's normally run.)
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import mcp_server  # noqa: E402
from kilnsim import payloads as pl  # noqa: E402
from kilnsim.cli import main  # noqa: E402
from kilnsim.link import MockSimLink  # noqa: E402
from kilnsim.protocol import CommandGroup  # noqa: E402


def _base_payload(duration, repeat=None):
    payload = {
        "fault_slot": 3,
        "fault_type": 0,
        "target": 1,
        "trigger": {"kind": "manual"},
        "duration": duration,
    }
    if repeat is not None:
        payload["repeat"] = repeat
    return payload


class DurationShapeEncodingTests(unittest.TestCase):
    """All three accepted `duration` shapes must produce identical bytes."""

    def test_dict_string_and_int_kind_are_byte_identical(self):
        dict_req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}))
        string_req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload("permanent"))
        int_req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload(0))  # 0 == permanent
        self.assertEqual(dict_req, string_req)
        self.assertEqual(dict_req, int_req)

    def test_missing_duration_defaults_to_permanent(self):
        payload = {
            "fault_slot": 3,
            "fault_type": 0,
            "target": 1,
            "trigger": {"kind": "manual"},
        }
        req = pl.encode_request(CommandGroup.FAULT, 1, payload)
        expected = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}))
        self.assertEqual(req, expected)

    def test_none_duration_defaults_to_permanent(self):
        req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload(None))
        expected = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}))
        self.assertEqual(req, expected)

    def test_bare_string_for_kind_preserves_kind_byte(self):
        # duration_kind byte sits right after the 44-byte ARM trigger block
        # (1 cmd + 2 slot + 1 fault_type + 2 target + 44 trigger = 50).
        req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload("until_trigger"))
        self.assertEqual(req[50], 2)


class RepeatShapeEncodingTests(unittest.TestCase):
    """`repeat` has the identical dict-only shape as `duration` and the same
    latent exposure -- kilnsim.mcp_server.fault_schedule's
    `repeat or "once"` fallback (mcp_server.py, pre-fix) passed a bare string
    for a missing `repeat` too, so this was not a theoretical-only risk."""

    def test_dict_string_and_int_kind_are_byte_identical(self):
        dict_req = pl.encode_request(
            CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat={"kind": "once"})
        )
        string_req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat="once"))
        int_req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat=0))
        self.assertEqual(dict_req, string_req)
        self.assertEqual(dict_req, int_req)

    def test_missing_repeat_defaults_to_once(self):
        req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}))
        expected = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat={"kind": "once"}))
        self.assertEqual(req, expected)

    def test_none_repeat_defaults_to_once(self):
        req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat=None))
        expected = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat={"kind": "once"}))
        self.assertEqual(req, expected)

    def test_bare_string_every_preserves_period_and_jitter_default_zero(self):
        # A bare-string repeat kind can't carry period/jitter/n (those only
        # exist on the dict form) -- confirm it still encodes cleanly with
        # those fields defaulted to zero, rather than raising.
        req = pl.encode_request(CommandGroup.FAULT, 1, _base_payload({"kind": "permanent"}, repeat="every"))
        # repeat_kind_id byte follows duration_kind(1)+duration_for_s(8) at
        # offset 50 (see test above) + 1 + 8 = 59.
        self.assertEqual(req[59], 1)  # EVERY == 1


class CliFaultSubcommandEndToEndTests(unittest.TestCase):
    """`kilnsim fault <target> <type>` through the real CLI entry point in
    --mock mode -- the exact path that crashed with AttributeError on every
    invocation before the fix (cli.py built `"duration": "permanent"`, a
    bare string, and MockSimLink's `_default_response` called
    `duration.get("kind")` on it unconditionally)."""

    def run_cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_fault_manual_trigger_succeeds(self):
        code, out, err = self.run_cli(["--mock", "fault", "relay:K1", "welded_ssr"])
        self.assertEqual(code, 0, msg=f"stderr: {err}")
        self.assertIn("fault scheduled: welded_ssr on relay:K1", out)

    def test_fault_at_temp_trigger_succeeds(self):
        code, out, err = self.run_cli(
            ["--mock", "fault", "tc0", "tc_disconnected", "--zone", "0", "--at-temp", "250"]
        )
        self.assertEqual(code, 0, msg=f"stderr: {err}")
        self.assertIn("fault scheduled: tc_disconnected on tc0", out)


class McpServerFaultScheduleTests(unittest.TestCase):
    """kilnsim.mcp_server.fault_schedule's own broken fallback
    (`duration or "permanent"` / `repeat or "once"`) against MockSimLink --
    no live link required."""

    def setUp(self):
        mcp_server._link = MockSimLink()
        mcp_server._link.connect("MOCK")

    def tearDown(self):
        if mcp_server._link.is_connected:
            mcp_server._link.disconnect()

    def test_fault_schedule_defaults_do_not_error(self):
        result = mcp_server.fault_schedule(
            fault_type="welded_ssr",
            target="relay:K1",
            trigger={"kind": "manual"},
        )
        self.assertFalse(result.startswith("error:"), msg=result)
        self.assertIn("fault slot:", result)

    def test_fault_schedule_explicit_dict_duration_and_repeat(self):
        result = mcp_server.fault_schedule(
            fault_type="welded_ssr",
            target="relay:K1",
            trigger={"kind": "manual"},
            duration={"kind": "for", "t": 0.5},
            repeat={"kind": "n_times", "n": 3, "period": 1.0},
        )
        self.assertFalse(result.startswith("error:"), msg=result)
        self.assertIn("fault slot:", result)


if __name__ == "__main__":
    unittest.main()
