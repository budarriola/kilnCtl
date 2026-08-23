#!/usr/bin/env python3
"""Byte-exact cross-check of kilnsim.payloads (SimFW command-PAYLOAD
encode/decode) against
firmware/SimFW/test/vectors/cmd_payload_vectors.json -- the shared manifest
that closes the gap benchproto_frame_vectors.json's own convention left open:
that manifest only proves the frame *envelope* (header/CRC/reliability)
byte-identical between kilnsim.benchproto_codec and benchproto_frame.c (see
test_kilnsim_benchproto_codec.py). Every COMMAND PAYLOAD layout *inside* that
envelope was, until this manifest existed, duplicated by hand in this
module and in firmware/SimFW/src/tasks/cmd_task.c with nothing cross-checking
them.

Mirrors test_kilnsim_benchproto_codec.py's own structure: load the JSON,
encode/decode through the real production module (kilnsim.payloads), assert
byte-exact hex equality. The manifest's vectors are hand-derived from
docs/PROTOCOL.md's documented wire layout via a standalone struct.pack
computation (see the vectors file's own header comment) -- neither this test
nor firmware/SimFW/test/test_cmd_payload_vectors.c generated the manifest
itself, so a bug shared between payloads.py and cmd_task.c would still show
up as a mismatch here.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import payloads as pl  # noqa: E402
from kilnsim.protocol import CommandGroup  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
VECTORS_PATH = os.path.join(
    REPO_ROOT, "firmware", "SimFW", "test", "vectors", "cmd_payload_vectors.json"
)

_GROUP_NAME_TO_ENUM = {
    "sys": CommandGroup.SYS,
    "io": CommandGroup.IO,
    "tc_float_sample": CommandGroup.TC,
}


def _load_vectors():
    with open(VECTORS_PATH, "r", encoding="utf-8") as fh:
        return json.load(fh)


class RequestVectorTests(unittest.TestCase):
    """encode_request(group, cmd, fields) must produce exactly payload_hex,
    for every request vector in every group section of the manifest."""

    def test_every_request_vector(self):
        data = _load_vectors()
        for group_key, group_enum in _GROUP_NAME_TO_ENUM.items():
            section = data[group_key]
            for v in section["requests"]:
                with self.subTest(group=group_key, name=v["name"]):
                    encoded = pl.encode_request(group_enum, v["cmd"], v["fields"])
                    self.assertEqual(
                        encoded.hex(),
                        v["payload_hex"],
                        f"{group_key}/{v['name']} (cmd={v['cmd_name']}): request mismatch",
                    )


class ReplyVectorTests(unittest.TestCase):
    """decode_reply(group, cmd, data) must either raise CommandStatusError
    with the vector's declared status (non-OK vectors) or return a dict
    that agrees with the vector's declared fields on every key the vector
    lists (OK vectors) -- decode_reply()'s output dict may carry additional
    keys the manifest does not enumerate (e.g. fw_version_major alongside
    fw_version); this only requires agreement on what the manifest asserts."""

    def test_every_reply_vector(self):
        data = _load_vectors()
        for group_key, group_enum in _GROUP_NAME_TO_ENUM.items():
            section = data[group_key]
            for v in section.get("replies", []):
                with self.subTest(group=group_key, name=v["name"]):
                    raw = bytes.fromhex(v["payload_hex"])
                    if v["status"] != "OK":
                        with self.assertRaises(pl.CommandStatusError) as ctx:
                            pl.decode_reply(group_enum, v["cmd"], raw)
                        self.assertEqual(ctx.exception.status, getattr(pl, f"STATUS_{v['status']}"))
                        continue
                    decoded = pl.decode_reply(group_enum, v["cmd"], raw)
                    for key, expected in v["fields"].items():
                        self.assertIn(key, decoded, f"{group_key}/{v['name']}: missing key {key!r}")
                        actual = decoded[key]
                        if isinstance(expected, float):
                            self.assertAlmostEqual(
                                actual, expected, places=6,
                                msg=f"{group_key}/{v['name']}: field {key!r} mismatch",
                            )
                        else:
                            self.assertEqual(
                                actual, expected, f"{group_key}/{v['name']}: field {key!r} mismatch"
                            )


class CorruptionSanityTests(unittest.TestCase):
    """Negative-test proof: this manifest-driven check is capable of
    failing. Not a permanent fixture of the suite's assertions -- exercised
    manually per the gap-closure task's mandatory negative-test step by
    flipping a byte in the manifest and re-running; kept here only as a
    lightweight in-repo demonstration that a wrong hex string IS caught."""

    def test_wrong_hex_is_rejected(self):
        encoded = pl.encode_request(CommandGroup.IO, 3, {"exp": 1, "pin": 15})  # READ
        self.assertNotEqual(encoded.hex(), "03010e")  # one nibble off from the real "03010f"
        self.assertEqual(encoded.hex(), "03010f")


if __name__ == "__main__":
    unittest.main()
