#!/usr/bin/env python3
"""Tests for kilnsim.mcp_server's DUT-power tools -- specifically that the
main-domain (J18) and safety-domain (J19) relay tools are independently
controllable and that no tool/code path in this module issues both a
DUT_POWER_SET and a DUT_POWER_SAFETY_SET from a single call (PROTOCOL.md
sec 5.5: two independent relays exist precisely so GND_Main/GND_Safty never
get bonded through a shared control path -- see BACKGROUND in the task that
added this test for the full reasoning).

Run with: python -m unittest discover -s tools/PcTools/tests
(pytest also collects this file directly, which is how it's normally run.)
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import mcp_server  # noqa: E402
from kilnsim.link import MockSimLink  # noqa: E402


class DutPowerToolsTests(unittest.TestCase):
    def setUp(self):
        mcp_server._link = MockSimLink()
        mcp_server._link.connect("MOCK")

    def tearDown(self):
        if mcp_server._link.is_connected:
            mcp_server._link.disconnect()

    def test_main_domain_defaults_on(self):
        reply = json.loads(mcp_server.dut_power_get())
        self.assertEqual(reply, {"on": True})

    def test_safety_domain_defaults_on(self):
        reply = json.loads(mcp_server.dut_power_safety_get())
        self.assertEqual(reply, {"on": True})

    def test_main_set_does_not_affect_safety(self):
        mcp_server.dut_power_set("off")
        self.assertEqual(json.loads(mcp_server.dut_power_get()), {"on": False})
        self.assertEqual(json.loads(mcp_server.dut_power_safety_get()), {"on": True})

    def test_safety_set_does_not_affect_main(self):
        mcp_server.dut_power_safety_set("off")
        self.assertEqual(json.loads(mcp_server.dut_power_safety_get()), {"on": False})
        self.assertEqual(json.loads(mcp_server.dut_power_get()), {"on": True})

    def test_domains_independently_toggled_both_ways(self):
        mcp_server.dut_power_set("off")
        mcp_server.dut_power_safety_set("on")
        self.assertEqual(json.loads(mcp_server.dut_power_get()), {"on": False})
        self.assertEqual(json.loads(mcp_server.dut_power_safety_get()), {"on": True})

        mcp_server.dut_power_set("on")
        mcp_server.dut_power_safety_set("off")
        self.assertEqual(json.loads(mcp_server.dut_power_get()), {"on": True})
        self.assertEqual(json.loads(mcp_server.dut_power_safety_get()), {"on": False})

    def test_bad_state_rejected(self):
        result = mcp_server.dut_power_set("sideways")
        self.assertTrue(result.startswith("error:"))
        result = mcp_server.dut_power_safety_set("sideways")
        self.assertTrue(result.startswith("error:"))

    def test_no_tool_issues_both_domain_commands(self):
        """Guards the "no combined set-both" property at the tool-surface
        level: dut_power_set must only ever send IoCmd 6 (main SET), and
        dut_power_safety_set must only ever send IoCmd 9 (safety SET) -- one
        wire command per call, never both."""
        from kilnsim.protocol import CommandGroup, IoCmd

        link = mcp_server._link
        link._history.clear()
        mcp_server.dut_power_set("on")
        sent = [(g, c) for g, c, _p in link.sent_commands]
        self.assertEqual(sent, [(CommandGroup.IO, IoCmd.DUT_POWER_SET)])

        link._history.clear()
        mcp_server.dut_power_safety_set("on")
        sent = [(g, c) for g, c, _p in link.sent_commands]
        self.assertEqual(sent, [(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_SET)])


if __name__ == "__main__":
    unittest.main()
