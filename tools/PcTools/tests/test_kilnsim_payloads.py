#!/usr/bin/env python3
"""Round-trip tests for kilnsim.payloads -- per-command-group encode/decode
against firmware/SimFW/docs/PROTOCOL.md's byte layouts. No hardware: this
constructs request/reply bytes directly and checks the decode side recovers
what the encode side put in (a "does this codec agree with itself, and with
the documented byte offsets" check, not a cross-implementation proof --
PROTOCOL.md's payload layer has no shared vector manifest the way
benchproto's framing layer does).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import payloads as pl  # noqa: E402
from kilnsim.protocol import CommandGroup  # noqa: E402


def _ok_reply(fields: bytes = b"") -> bytes:
    return bytes([pl.STATUS_OK]) + fields


class SysGroupTests(unittest.TestCase):
    def test_ping_request(self):
        self.assertEqual(pl.encode_request(CommandGroup.SYS, 1), bytes([0x01]))

    def test_ping_reply(self):
        self.assertEqual(pl.decode_reply(CommandGroup.SYS, 1, _ok_reply()), {"pong": True})

    def test_get_version_reply(self):
        body = struct.pack("<HH", 1, 1) + struct.pack("<BBBBB", 0, 1, 2, 0, 3) + b"abc"
        reply = pl.decode_reply(CommandGroup.SYS, 2, _ok_reply(body))
        self.assertEqual(reply["protocol_version"], 1)
        self.assertEqual(reply["fw_version"], "0.1.2")
        self.assertFalse(reply["fw_git_dirty"])
        self.assertEqual(reply["fw_git_hash"], "abc")

    def test_get_caps_reply(self):
        version_block = struct.pack("<HH", 1, 1) + struct.pack("<BBBBB", 0, 1, 2, 0, 0) + b""
        caps_tail = struct.pack("<BBBBBBB", 1, 4, 3, 3, 1, 3, 5) + struct.pack("<I", 0x3F)
        reply = pl.decode_reply(CommandGroup.SYS, 6, _ok_reply(version_block + caps_tail))
        self.assertEqual(reply["zone_count_min"], 1)
        self.assertEqual(reply["zone_count_max"], 4)
        self.assertEqual(reply["tc_channel_count"], 3)
        self.assertEqual(reply["relay_count"], 5)
        self.assertEqual(reply["feature_bitmask"], 0x3F)

    def test_status_error_raises(self):
        with self.assertRaises(pl.CommandStatusError):
            pl.decode_reply(CommandGroup.SYS, 1, bytes([pl.STATUS_ERR_BAD_ARGS]))

    def test_empty_reply_raises_payload_error(self):
        with self.assertRaises(pl.PayloadError):
            pl.decode_reply(CommandGroup.SYS, 1, b"")


class ModelGroupTests(unittest.TestCase):
    def test_set_zone_params_round_trip_via_get(self):
        params = {
            "C": 1234.5,
            "k_loss": 0.5,
            "k_couple": [0.1, 0.2, 0.3, 0.4],
            "R_element": 10.0,
            "element_health": 1.0,
            "tc_lag_s": 2.5,
            "T0": 20.0,
        }
        req = pl.encode_request(CommandGroup.MODEL, 1, {"zone": 2, "params": params})
        self.assertEqual(req[0], 1)
        self.assertEqual(req[1], 2)

        packed = pl._pack_zone_params(params)
        reply_bytes = _ok_reply(bytes([2]) + packed)
        decoded = pl.decode_reply(CommandGroup.MODEL, 2, reply_bytes)
        self.assertEqual(decoded["zone"], 2)
        self.assertAlmostEqual(decoded["params"]["C"], 1234.5, places=1)
        self.assertEqual(len(decoded["params"]["k_couple"]), 4)

    def test_load_preset_by_name(self):
        req = pl.encode_request(CommandGroup.MODEL, 4, {"name": "three_zone"})
        self.assertEqual(req, bytes([4, 2]))

    def test_set_temp_manual_mode(self):
        req = pl.encode_request(CommandGroup.MODEL, 5, {"zone": 0, "mode": "manual", "temp_c": 900.0})
        self.assertEqual(req[0:3], bytes([5, 0, 1]))
        self.assertAlmostEqual(struct.unpack("<f", req[3:7])[0], 900.0, places=1)


class TcGroupTests(unittest.TestCase):
    def test_get_regs_reply(self):
        regs = b"\x00" * 16
        body = bytes([0, 0x02]) + regs + struct.pack("<ff", 500.0, 498.5) + bytes([1]) + struct.pack(
            "<ffIII", 0.1, 0.0001, 42, 0, 0
        )
        decoded = pl.decode_reply(CommandGroup.TC, 1, _ok_reply(body))
        self.assertEqual(decoded["channel"], 0)
        self.assertTrue(decoded["snapshot_valid"])
        self.assertAlmostEqual(decoded["shadow_temp_c"], 500.0, places=1)
        self.assertEqual(decoded["spi_transactions"], 42)

    def test_inject_fault_request_and_reply(self):
        req = pl.encode_request(
            CommandGroup.TC, 4, {"fault_slot": 7, "channel": 1, "fault_kind": 2, "param0": 0.0}
        )
        self.assertEqual(req[0], 4)
        reply = pl.decode_reply(CommandGroup.TC, 4, _ok_reply(struct.pack("<H", 7)))
        self.assertEqual(reply["fault_slot"], 7)

    def test_get_master_config_request(self):
        req = pl.encode_request(CommandGroup.TC, 6, {"channel": 2})
        self.assertEqual(req, bytes([6, 2]))

    def test_get_master_config_reply(self):
        # flags: bit0 configured=1, bit1 reg_image_valid=1 -> 0x03
        body = bytes([3, 0x03, 0x11, 0x22, 0x0F])  # channel, flags, cr0, cr1, mask
        decoded = pl.decode_reply(CommandGroup.TC, 6, _ok_reply(body))
        self.assertEqual(decoded["channel"], 3)
        self.assertTrue(decoded["configured"])
        self.assertTrue(decoded["reg_image_valid"])
        self.assertEqual(decoded["cr0"], 0x11)
        self.assertEqual(decoded["cr1"], 0x22)
        self.assertEqual(decoded["mask"], 0x0F)

    def test_get_master_config_reply_not_configured_and_busy(self):
        # flags 0x00: never configured, and this poll caught it mid-transaction
        body = bytes([1, 0x00, 0, 0, 0])
        decoded = pl.decode_reply(CommandGroup.TC, 6, _ok_reply(body))
        self.assertFalse(decoded["configured"])
        self.assertFalse(decoded["reg_image_valid"])


class CtGroupTests(unittest.TestCase):
    def test_get_state_reply(self):
        body = bytes([1]) + struct.pack("<ffff", 5.0, 0.0, 0.0, 0.0) + bytes([0, 0, 1]) + struct.pack(
            "<f", 1.0
        ) + bytes([1])
        decoded = pl.decode_reply(CommandGroup.CT, 4, _ok_reply(body))
        self.assertEqual(decoded["mode"], 1)
        self.assertAlmostEqual(decoded["amps"], 5.0, places=1)
        self.assertTrue(decoded["valid"])

    def test_set_distortion_request(self):
        req = pl.encode_request(
            CommandGroup.CT,
            3,
            {
                "channel": 0,
                "distortion": {
                    "dc_offset": 0.1,
                    "clip_fraction": 0.05,
                    "dropout_half_cycle": True,
                    "dropout_negative_half": False,
                    "apply_immediately": True,
                },
            },
        )
        self.assertEqual(len(req), 2 + 4 + 4 + 3)


class RelayGroupTests(unittest.TestCase):
    def test_get_states_reply(self):
        body = bytes([1, 0, 1, 0, 1]) + bytes([0]) + struct.pack("<Q", 12345) + bytes([1])
        decoded = pl.decode_reply(CommandGroup.RELAY, 1, _ok_reply(body))
        self.assertTrue(decoded["k1_closed"])
        self.assertFalse(decoded["k2_closed"])
        self.assertEqual(decoded["sample_time_us"], 12345)

    def test_get_edges_reply(self):
        entry = struct.pack("<IBB", 1, 0, 1) + struct.pack("<Q", 999)
        body = bytes([1]) + entry
        decoded = pl.decode_reply(CommandGroup.RELAY, 2, _ok_reply(body))
        self.assertEqual(decoded["returned_count"], 1)
        self.assertEqual(decoded["edges"][0]["signal"], "K1")
        self.assertEqual(decoded["edges"][0]["sim_time_us"], 999)

    def test_set_contact_fault_not_allocated(self):
        # PROTOCOL.md sec 5.4: RELAY_SET_CONTACT_FAULT is deliberately not
        # allocated on the wire -- no SIMFW_CMD_RELAY_* id 3 exists.
        with self.assertRaises(pl.PayloadError):
            pl.encode_request(CommandGroup.RELAY, 3, {})


class IoGroupTests(unittest.TestCase):
    def test_read_reply(self):
        decoded = pl.decode_reply(CommandGroup.IO, 3, _ok_reply(bytes([1])))
        self.assertTrue(decoded["level"])

    def test_estop_set_request(self):
        req = pl.encode_request(CommandGroup.IO, 4, {"open": True})
        self.assertEqual(req, bytes([4, 1]))

    def test_fault_line_get_reply(self):
        body = bytes([1]) + struct.pack("<Q", 555) + bytes([1])
        decoded = pl.decode_reply(CommandGroup.IO, 5, _ok_reply(body))
        self.assertTrue(decoded["asserted"])
        self.assertEqual(decoded["sample_time_us"], 555)

    def test_dut_power_set_request(self):
        req = pl.encode_request(CommandGroup.IO, 6, {"on": True})
        self.assertEqual(req, bytes([6, 1]))

    def test_estop_get_request_and_reply(self):
        req = pl.encode_request(CommandGroup.IO, 7, {})
        self.assertEqual(req, bytes([7]))
        decoded = pl.decode_reply(CommandGroup.IO, 7, _ok_reply(bytes([1])))
        self.assertTrue(decoded["open"])

    def test_dut_power_get_request_and_reply(self):
        req = pl.encode_request(CommandGroup.IO, 8, {})
        self.assertEqual(req, bytes([8]))
        decoded = pl.decode_reply(CommandGroup.IO, 8, _ok_reply(bytes([0])))
        self.assertFalse(decoded["on"])


class FaultGroupTests(unittest.TestCase):
    def test_schedule_manual_permanent_once(self):
        req = pl.encode_request(
            CommandGroup.FAULT,
            1,
            {
                "fault_slot": 3,
                "fault_type": 0,
                "target": 1,
                "trigger": {"kind": "manual"},
                "duration": {"kind": "permanent"},
                "repeat": {"kind": "once"},
            },
        )
        self.assertEqual(req[0], 1)
        # slot_id at offset 1..2
        self.assertEqual(struct.unpack_from("<H", req, 1)[0], 3)
        self.assertEqual(len(req), 94)

    def test_schedule_until_trigger_accepted_frame1(self):
        # PROTOCOL.md sec 5.6: duration_kind == 2 (UNTIL_TRIGGER) is now
        # *accepted* by FAULT_SCHEDULE -- it's frame 1 of the two-frame
        # design, parking the fault's fields without arming the slot yet.
        # Same 94-byte wire shape as PERMANENT/FOR; duration_for_s is simply
        # ignored (not encoded specially).
        req = pl.encode_request(
            CommandGroup.FAULT,
            1,
            {
                "fault_slot": 1,
                "fault_type": 0,
                "target": 1,
                "trigger": {"kind": "manual"},
                "duration": {"kind": "until_trigger"},
                "repeat": {"kind": "once"},
            },
        )
        self.assertEqual(req[0], 1)
        self.assertEqual(len(req), 94)
        # duration_kind byte sits right after the 44-byte ARM trigger block
        # (1 cmd + 2 slot + 1 fault_type + 2 target + 44 trigger = 50).
        self.assertEqual(req[50], 2)

    def test_set_until_trigger_request_round_trip(self):
        # Frame 2: [0x05, u16 slot_id, <trigger encoding>], 47 bytes total.
        req = pl.encode_request(
            CommandGroup.FAULT,
            5,
            {
                "fault_slot": 7,
                "trigger": {"kind": "at_zone_temp", "zone": 2, "temp_c": 250.0, "edge": "falling"},
            },
        )
        self.assertEqual(req[0], 5)
        self.assertEqual(len(req), 47)
        self.assertEqual(struct.unpack_from("<H", req, 1)[0], 7)
        # trigger_kind (AT_ZONE_TEMP == 1) at offset 3
        self.assertEqual(req[3], 1)
        # trigger_a (temp_c) at offset 4, f64
        self.assertAlmostEqual(struct.unpack_from("<d", req, 4)[0], 250.0, places=3)
        # trigger_ref (zone) at offset 20, u16
        self.assertEqual(struct.unpack_from("<H", req, 20)[0], 2)
        # trigger_edge (falling == 1) at offset 22
        self.assertEqual(req[22], 1)

        decoded = pl.decode_reply(CommandGroup.FAULT, 5, _ok_reply(struct.pack("<H", 7)))
        self.assertEqual(decoded, {"fault_slot": 7})

    def test_set_until_trigger_matches_schedule_arm_trigger_encoding(self):
        # "byte-identical trigger encoding to FAULT_SCHEDULE's own ARM
        # trigger" (PROTOCOL.md sec 5.6) -- same trigger dict through both
        # encoders should produce the identical 44-byte trigger block.
        trigger = {"kind": "on_relay_edge", "relay": "K4", "edge": "open", "delay_s": 1.5}
        schedule_req = pl.encode_request(
            CommandGroup.FAULT,
            1,
            {
                "fault_slot": 0,
                "fault_type": 0,
                "target": 0,
                "trigger": trigger,
                "duration": {"kind": "permanent"},
                "repeat": {"kind": "once"},
            },
        )
        until_req = pl.encode_request(CommandGroup.FAULT, 5, {"fault_slot": 0, "trigger": trigger})
        # schedule_req's ARM trigger block is at offset 6..50 (1 cmd + 2 slot
        # + 1 fault_type + 2 target = 6); until_req's is at offset 3..47
        # (1 cmd + 2 slot_id = 3).
        self.assertEqual(schedule_req[6:50], until_req[3:47])

    def test_list_reply(self):
        entry = struct.pack("<HBHHI", 1, 1, 2, 0, 5) + struct.pack("<f", 12.5)
        body = bytes([1]) + entry
        decoded = pl.decode_reply(CommandGroup.FAULT, 3, _ok_reply(body))
        self.assertEqual(decoded["returned_count"], 1)
        self.assertEqual(decoded["faults"][0]["state"], "armed")
        self.assertEqual(decoded["faults"][0]["fire_count"], 5)


class EvtBroadcastTests(unittest.TestCase):
    def test_decode_evt_frame(self):
        body = bytes([pl.EVT_FRAME_KIND_EVENT]) + struct.pack("<IQBBBf", 9, 42, 1, 5, 0, 3.5)
        decoded = pl.decode_evt_frame(body)
        self.assertEqual(decoded["seq"], 9)
        self.assertEqual(decoded["sim_time_us"], 42)
        self.assertEqual(decoded["event_type"], 1)
        self.assertEqual(decoded["a"], 5)
        self.assertAlmostEqual(decoded["f0"], 3.5, places=3)

    def test_decode_telemetry_frame(self):
        zone = struct.pack("<ffff", 500.0, 498.0, 495.0, 10.0)
        body = (
            bytes([pl.EVT_FRAME_KIND_TELEMETRY])
            + struct.pack("<QIIB", 1000, 100, 0, 1)
            + zone
            + struct.pack("<HBBH", 0b101, 0, 1, 2)
            + struct.pack("<IIIII", 10, 0, 3, 1, 0)
        )
        decoded = pl.decode_telemetry_frame(body)
        self.assertEqual(decoded["sim_time_us"], 1000)
        self.assertEqual(decoded["timescale"], 1.0)
        self.assertEqual(len(decoded["zones"]), 1)
        self.assertAlmostEqual(decoded["zones"][0]["t_zone"], 500.0, places=1)
        self.assertEqual(decoded["relay_state_mask"], 0b101)
        self.assertTrue(decoded["fault_line_asserted"])
        self.assertEqual(decoded["active_fault_count"], 2)
        self.assertEqual(decoded["evt_seq_gap_count"], 1)


if __name__ == "__main__":
    unittest.main()
