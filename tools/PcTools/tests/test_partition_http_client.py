#!/usr/bin/env python3
"""Unit tests for kilnctrl.partition_http_client -- the HTTP client backing
GET /api/partitions, FLASH_BUDGET_PLAN.md section 8 item 3's replacement for
the broken JTAG-based partition table read. All against MOCKED urllib
responses -- no real socket, no live board. Same shape as
test_dashboard_http_client.py's GetHeapStatusTest.

Run with: python -m pytest tools/PcTools/tests/test_partition_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import partition_http_client as phc  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _sample_body(**overrides) -> dict:
    body = {
        "running": "ota_0",
        "partitions": [
            {"label": "nvs", "type": 1, "subtype": 2, "offset": 36864, "size": 24576, "encrypted": False},
            {"label": "ota_0", "type": 0, "subtype": 16, "offset": 65536, "size": 1048576, "encrypted": False},
            {"label": "coredump", "type": 1, "subtype": 3, "offset": 2162688, "size": 65536, "encrypted": True},
        ],
    }
    body.update(overrides)
    return body


class GetPartitionsTest(unittest.TestCase):
    def test_parses_running_and_every_entry(self):
        body = json.dumps(_sample_body()).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            data = phc.get_partitions("192.168.1.156")
        self.assertEqual(data["running"], "ota_0")
        self.assertEqual(len(data["partitions"]), 3)
        by_label = {p["label"]: p for p in data["partitions"]}
        self.assertEqual(by_label["coredump"]["size"], 65536)
        self.assertTrue(by_label["coredump"]["encrypted"])
        self.assertFalse(by_label["nvs"]["encrypted"])

    def test_empty_partitions_list_is_not_an_error(self):
        body = json.dumps(_sample_body(partitions=[])).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            data = phc.get_partitions("192.168.1.156")
        self.assertEqual(data["partitions"], [])

    def test_missing_running_key_raises_not_silently_empty(self):
        """NEGATIVE TEST: a response missing the top-level "running" key must
        be refused loudly, not read back as running="" (which would look
        identical to the legitimate "no OTA slot known" case) or silently
        dropped. Proves this check can actually fail: delete the key from
        the fixture and watch the assertion below go red before it is
        restored."""
        body = _sample_body()
        del body["running"]
        raw = json.dumps(body).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(raw)):
            with self.assertRaises(phc.PartitionHttpError) as ctx:
                phc.get_partitions("192.168.1.156")
        self.assertIn("running", str(ctx.exception))

    def test_entry_missing_required_field_raises(self):
        body = _sample_body()
        del body["partitions"][1]["offset"]  # ota_0 entry loses its offset field
        raw = json.dumps(body).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(raw)):
            with self.assertRaises(phc.PartitionHttpError) as ctx:
                phc.get_partitions("192.168.1.156")
        self.assertIn("offset", str(ctx.exception))

    def test_partitions_not_a_list_raises(self):
        body = _sample_body(partitions={"not": "a list"})
        raw = json.dumps(body).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(raw)):
            with self.assertRaises(phc.PartitionHttpError):
                phc.get_partitions("192.168.1.156")

    def test_http_error_surfaced_as_partition_http_error(self):
        with unittest.mock.patch(
            "urllib.request.urlopen", side_effect=OSError("connection refused")
        ):
            with self.assertRaises(phc.PartitionHttpError):
                phc.get_partitions("192.168.1.156")

    def test_non_json_body_surfaced_as_partition_http_error(self):
        body = b"not json"
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            with self.assertRaises(phc.PartitionHttpError):
                phc.get_partitions("192.168.1.156")


if __name__ == "__main__":
    unittest.main()
