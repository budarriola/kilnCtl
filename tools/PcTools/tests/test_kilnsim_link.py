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
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import link as kilnsim_link  # noqa: E402
from kilnsim.link import MockSimLink, SerialSimLink, SimLinkError  # noqa: E402
from kilnsim.protocol import CommandGroup, Event, EventType, FaultCmd, IoCmd, SysCmd, TcCmd  # noqa: E402


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

    def test_tc_get_master_config_has_expected_shape(self):
        reply = self.link.send_command(CommandGroup.TC, TcCmd.GET_MASTER_CONFIG, {"channel": 1})
        self.assertEqual(reply["channel"], 1)
        for key in ("configured", "reg_image_valid", "cr0", "cr1", "mask"):
            self.assertIn(key, reply)

    def test_history_records_every_send(self):
        self.link.send_command(CommandGroup.SYS, SysCmd.PING)
        self.link.send_command(CommandGroup.MODEL, 4, {"name": "fast_test"})
        self.assertEqual(len(self.link.sent_commands), 2)
        self.assertEqual(self.link.sent_commands[1], (CommandGroup.MODEL, 4, {"name": "fast_test"}))


class DutPowerDomainTests(unittest.TestCase):
    """MockSimLink must track main (J18, cmd 6/8) and safety (J19, cmd 9/10)
    DUT-power relay state independently -- a mock that answered both GETs
    from one shared flag would hide exactly the main/safety mix-up bug this
    feature exists to catch (PROTOCOL.md sec 5.5)."""

    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_both_domains_default_on(self):
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET), {"on": True}
        )
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET), {"on": True}
        )

    def test_main_set_only_affects_main(self):
        self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": False})
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET), {"on": False}
        )
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET), {"on": True}
        )

    def test_safety_set_only_affects_safety(self):
        self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_SET, {"on": False})
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET), {"on": False}
        )
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET), {"on": True}
        )

    def test_domains_can_hold_opposite_states_simultaneously(self):
        self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": False})
        self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_SET, {"on": True})
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET), {"on": False}
        )
        self.assertEqual(
            self.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET), {"on": True}
        )


class ScriptedResponseTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_scripted_response_overrides_default(self):
        self.link.script_response(CommandGroup.SYS, SysCmd.PING, {"pong": False, "note": "custom"})
        reply = self.link.send_command(CommandGroup.SYS, SysCmd.PING)
        self.assertEqual(reply, {"pong": False, "note": "custom"})

    def test_scripted_responses_are_consumed_fifo(self):
        # MODEL/1 (SET_ZONE_PARAMS) has no special-cased mock default, so the
        # third call exercises the generic-ack fallback path.
        self.link.script_response(CommandGroup.MODEL, 1, {"channel": 0, "seq": 1})
        self.link.script_response(CommandGroup.MODEL, 1, {"channel": 0, "seq": 2})
        first = self.link.send_command(CommandGroup.MODEL, 1)
        second = self.link.send_command(CommandGroup.MODEL, 1)
        third = self.link.send_command(CommandGroup.MODEL, 1)  # falls back to default (generic ack)
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


class FaultUntilTriggerTwoFrameTests(unittest.TestCase):
    """MockSimLink's answer for the UNTIL_TRIGGER two-frame design
    (PROTOCOL.md sec 5.6), so scenario/runner tests built on the mock stay
    meaningful without a real device."""

    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_set_until_trigger_after_pending_schedule_succeeds(self):
        self.link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": 2, "duration": {"kind": "until_trigger"},
        })
        reply = self.link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
            "fault_slot": 2, "trigger": {"kind": "at_sim_time", "t": 5.0},
        })
        self.assertEqual(reply, {"fault_slot": 2})

    def test_set_until_trigger_with_nothing_pending_raises(self):
        with self.assertRaises(SimLinkError):
            self.link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
                "fault_slot": 9, "trigger": {"kind": "manual"},
            })

    def test_set_until_trigger_twice_second_call_raises(self):
        # Once consumed by a SET_UNTIL_TRIGGER, the pending entry is gone --
        # a second SET_UNTIL_TRIGGER for the same slot has nothing pending.
        self.link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": 1, "duration": {"kind": "until_trigger"},
        })
        self.link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
            "fault_slot": 1, "trigger": {"kind": "manual"},
        })
        with self.assertRaises(SimLinkError):
            self.link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
                "fault_slot": 1, "trigger": {"kind": "manual"},
            })

    def test_permanent_schedule_discards_stale_pending_entry(self):
        self.link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": 3, "duration": {"kind": "until_trigger"},
        })
        # A direct PERMANENT FAULT_SCHEDULE on the same slot discards the
        # stale pending UNTIL_TRIGGER entry (PROTOCOL.md sec 5.6).
        self.link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": 3, "duration": {"kind": "permanent"},
        })
        with self.assertRaises(SimLinkError):
            self.link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
                "fault_slot": 3, "trigger": {"kind": "manual"},
            })

    def test_cancel_discards_pending_entry(self):
        self.link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": 4, "duration": {"kind": "until_trigger"},
        })
        self.link.send_command(CommandGroup.FAULT, FaultCmd.CANCEL, {"fault_slot": 4})
        with self.assertRaises(SimLinkError):
            self.link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
                "fault_slot": 4, "trigger": {"kind": "manual"},
            })


class EventRoundTripTests(unittest.TestCase):
    def test_event_to_dict_from_dict_round_trip(self):
        evt = Event(seq=9, sim_time_us=42, event_type=EventType.DUT_POWER, payload={"on": False})
        restored = Event.from_dict(evt.to_dict())
        self.assertEqual(evt, restored)


def _fake_port(device, hwid="", interface=None, description="", product=""):
    """A pyserial ListPortInfo-shaped stand-in -- SimpleNamespace, since
    SerialSimLink._port_mentions()/_list_candidate_port_infos() only ever
    read .hwid/.interface/.description/.product/.device via getattr()."""
    return SimpleNamespace(device=device, hwid=hwid, interface=interface,
                            description=description, product=product)


class ProtocolPortDiscoveryTests(unittest.TestCase):
    """SerialSimLink.list_protocol_ports() -- the dual-CDC port-disambiguation
    logic added 2026-08-23 alongside SimFW's new console CDC
    (firmware/SimFW/docs/PROTOCOL.md sec 1). Both CDC ports share one VID:PID
    now, so list_candidate_ports() alone can no longer tell them apart;
    these tests pin the three-tier fallback (interface string -> Windows
    MI_00 -> "give up, return everything") by faking pyserial's
    list_ports.comports() return value -- no real hardware or pyserial I/O.
    """

    def setUp(self):
        patcher = mock.patch.object(kilnsim_link, "_list_ports")
        self.mock_list_ports = patcher.start()
        self.addCleanup(patcher.stop)

    def _set_comports(self, ports):
        self.mock_list_ports.comports.return_value = ports

    def test_prefers_the_interface_string_when_available(self):
        console = _fake_port("COM5", hwid="USB VID:PID=2E8A:F00A", interface="SimFW Console")
        protocol = _fake_port("COM6", hwid="USB VID:PID=2E8A:F00A", interface="SimFW Control")
        self._set_comports([console, protocol])
        self.assertEqual(SerialSimLink.list_protocol_ports(), ["COM6"])

    def test_matches_the_string_in_description_or_product_too(self):
        # Not every OS/pyserial backend populates .interface -- description
        # and product are the other two fields real composite-CDC friendly
        # names commonly carry the interface string in.
        protocol = _fake_port("COM7", hwid="USB VID:PID=2E8A:F00A", description="SimFW Control (COM7)")
        self._set_comports([protocol])
        self.assertEqual(SerialSimLink.list_protocol_ports(), ["COM7"])

    def test_falls_back_to_windows_mi00_when_no_interface_string_visible(self):
        # Neither port exposes the interface string at all (interface/
        # description/product all empty) -- the Windows composite-device
        # hwid convention (MI_00 == USB function/interface number 0, always
        # the protocol CDC's control interface per usb_descriptors.c's
        # declaration order) is the fallback.
        console = _fake_port("COM5", hwid="USB VID:PID=2E8A:F00A&MI_02")
        protocol = _fake_port("COM6", hwid="USB VID:PID=2E8A:F00A&MI_00")
        self._set_comports([console, protocol])
        self.assertEqual(SerialSimLink.list_protocol_ports(), ["COM6"])

    def test_last_resort_returns_every_vid_pid_match(self):
        # Nothing distinguishes the two ports at all (no interface string,
        # no MI_XX in hwid) -- the final fallback is "everything that
        # matched VID:PID", same as the pre-dual-CDC behavior, rather than
        # silently guessing one.
        a = _fake_port("COM5", hwid="USB VID:PID=2E8A:F00A")
        b = _fake_port("COM6", hwid="USB VID:PID=2E8A:F00A")
        self._set_comports([a, b])
        self.assertEqual(sorted(SerialSimLink.list_protocol_ports()), ["COM5", "COM6"])

    def test_ignores_ports_with_a_different_vid_pid(self):
        # Deliberately gives the WRONG-VID:PID device the exact interface
        # string too -- so this only passes if the VID:PID gate is applied
        # BEFORE the interface-string match, not because the string match
        # alone happened to prefer the right port.
        other_device = _fake_port("COM3", hwid="USB VID:PID=1234:5678", interface="SimFW Control")
        protocol = _fake_port("COM6", hwid="USB VID:PID=2E8A:F00A", interface="SimFW Control")
        self._set_comports([other_device, protocol])
        self.assertEqual(SerialSimLink.list_protocol_ports(), ["COM6"])

    def test_list_candidate_ports_still_returns_both_cdc_ports(self):
        # list_candidate_ports() is the "everything at this VID:PID" view
        # (kept for backward compatibility / diagnostics) -- it must NOT be
        # narrowed to just the protocol port, or a caller relying on it to
        # see the whole fixture would silently lose visibility into CDC1.
        console = _fake_port("COM5", hwid="USB VID:PID=2E8A:F00A", interface="SimFW Console")
        protocol = _fake_port("COM6", hwid="USB VID:PID=2E8A:F00A", interface="SimFW Control")
        self._set_comports([console, protocol])
        self.assertEqual(sorted(SerialSimLink.list_candidate_ports()), ["COM5", "COM6"])


if __name__ == "__main__":
    unittest.main()
