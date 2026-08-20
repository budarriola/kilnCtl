#!/usr/bin/env python3
"""Tests for kilnsim.link.MockSimLink -- the in-memory SimLink test double
every other kilnsim module's tests run against. No hardware, no pyserial
I/O -- direct call/return.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.link import MockSimLink, SimLinkError  # noqa: E402
from kilnsim.protocol import CommandGroup, Event, EventType, SysCmd  # noqa: E402


class ConnectDisconnectTests(unittest.TestCase):
    def test_starts_disconnected(self):
        link = MockSimLink()
        self.assertFalse(link.is_connected)

    def test_connect_reports_connected(self):
        link = MockSimLink()
        port = link.connect()
        self.assertTrue(link.is_connected)
        self.assertEqual(port, "MOCK")

    def test_connect_twice_raises(self):
        link = MockSimLink()
        link.connect()
        with self.assertRaises(SimLinkError):
            link.connect()

    def test_send_before_connect_raises(self):
        link = MockSimLink()
        with self.assertRaises(SimLinkError):
            link.send_command(CommandGroup.SYS, SysCmd.PING)

    def test_disconnect_then_reconnect(self):
        link = MockSimLink()
        link.connect()
        link.disconnect()
        self.assertFalse(link.is_connected)
        link.connect()
        self.assertTrue(link.is_connected)


class BuiltInDefaultResponseTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_ping(self):
        reply = self.link.send_command(CommandGroup.SYS, SysCmd.PING)
        self.assertEqual(reply, {"pong": True})

    def test_get_caps_has_expected_shape(self):
        reply = self.link.send_command(CommandGroup.SYS, SysCmd.GET_CAPS)
        for key in ("protocol_version", "zone_count_min", "zone_count_max",
                    "tc_channel_count", "ct_channel_count", "relay_count"):
            self.assertIn(key, reply)

    def test_unscripted_command_falls_back_to_generic_ack(self):
        reply = self.link.send_command(CommandGroup.RELAY, 3, {"anything": 1})
        self.assertEqual(reply, {"ok": True})

    def test_history_records_every_send(self):
        self.link.send_command(CommandGroup.SYS, SysCmd.PING)
        self.link.send_command(CommandGroup.MODEL, 4, {"name": "fast_test"})
        self.assertEqual(len(self.link.sent_commands), 2)
        self.assertEqual(self.link.sent_commands[1], (CommandGroup.MODEL, 4, {"name": "fast_test"}))


class ScriptedResponseTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_scripted_response_overrides_default(self):
        self.link.script_response(CommandGroup.SYS, SysCmd.PING, {"pong": False, "note": "custom"})
        reply = self.link.send_command(CommandGroup.SYS, SysCmd.PING)
        self.assertEqual(reply, {"pong": False, "note": "custom"})

    def test_scripted_responses_are_consumed_fifo(self):
        self.link.script_response(CommandGroup.TC, 1, {"channel": 0, "seq": 1})
        self.link.script_response(CommandGroup.TC, 1, {"channel": 0, "seq": 2})
        first = self.link.send_command(CommandGroup.TC, 1)
        second = self.link.send_command(CommandGroup.TC, 1)
        third = self.link.send_command(CommandGroup.TC, 1)  # falls back to default (generic ack)
        self.assertEqual(first["seq"], 1)
        self.assertEqual(second["seq"], 2)
        self.assertEqual(third, {"ok": True})

    def test_scripted_error_raises_simlinkerror(self):
        self.link.script_response(CommandGroup.FAULT, 1, {}, error="slot pool exhausted")
        with self.assertRaises(SimLinkError):
            self.link.send_command(CommandGroup.FAULT, 1)


class EventInjectionTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_read_events_empty_by_default(self):
        self.assertEqual(self.link.read_events(), [])

    def test_injected_event_is_returned_once(self):
        evt = Event(seq=1, sim_time_us=1000, event_type=EventType.RELAY_EDGE, payload={"entity": "K1", "state": True})
        self.link.inject_event(evt)
        first = self.link.read_events()
        second = self.link.read_events()
        self.assertEqual(first, [evt])
        self.assertEqual(second, [])

    def test_make_event_auto_increments_seq(self):
        e1 = self.link.make_event(EventType.FAULT_FIRED, {"fault_id": "weld"})
        e2 = self.link.make_event(EventType.FAULT_CLEARED, {"fault_id": "weld"})
        self.assertEqual(e1.seq, 1)
        self.assertEqual(e2.seq, 2)
        events = self.link.read_events()
        self.assertEqual(events, [e1, e2])

    def test_make_event_defaults_sim_time_to_current_state(self):
        self.link.set_state(sim_time_us=555)
        evt = self.link.make_event(EventType.SIM_CLOCK_MARK)
        self.assertEqual(evt.sim_time_us, 555)


class StateSnapshotTests(unittest.TestCase):
    def test_default_snapshot_has_one_zone(self):
        link = MockSimLink()
        snap = link.get_state_snapshot()
        self.assertEqual(len(snap["zones"]), 1)

    def test_set_state_updates_snapshot(self):
        link = MockSimLink()
        link.set_state(estop_open=True)
        self.assertTrue(link.get_state_snapshot()["estop_open"])


class EventRoundTripTests(unittest.TestCase):
    def test_event_to_dict_from_dict_round_trip(self):
        evt = Event(seq=9, sim_time_us=42, event_type=EventType.DUT_POWER, payload={"on": False})
        restored = Event.from_dict(evt.to_dict())
        self.assertEqual(evt, restored)


if __name__ == "__main__":
    unittest.main()
