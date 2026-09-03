#!/usr/bin/env python3
"""Unit tests for ota_status()'s protocol-version-compatibility reporting
(mcp_server_ota.py) -- TODO.md's "a protocol-version mismatch between a Pico
image and the running ESP is a hard error here, not a warning" and
"ota_status() reports both processors' protocol version and min_compatible,
and says plainly whether they are compatible and which side is older".

The wire fields (protocol_version_known/protocol_version/
protocol_min_compatible/protocol_compatible) are already emitted by
ota_http.c's ota_pico_status_get_handler() and passed through unmodified by
ota_http_client.get_pico_status() (plain json.loads) -- what was missing was
ota_status() actually reading and surfacing them. All against a MOCKED
ota_http_client -- no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_ota_status_protocol_version.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota as mo  # noqa: E402


class OtaStatusProtocolVersionTest(unittest.TestCase):
    def _patch(self, pico_status: dict, esp_status: dict | None = None):
        esp_status = esp_status if esp_status is not None else {
            "phase": "idle", "percent": 0, "last_update": None}
        return unittest.mock.patch.multiple(
            mo.ota_http,
            get_pico_status=unittest.mock.Mock(return_value=pico_status),
            get_esp_status=unittest.mock.Mock(return_value=esp_status),
        )

    def test_compatible_versions_reported_plainly(self):
        pico_status = {
            "phase": "idle", "percent": 0, "last_error": "",
            "protocol_version_known": True,
            "protocol_version": 3,
            "protocol_min_compatible": 2,
            "protocol_compatible": True,
        }
        with self._patch(pico_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("protocol_version=3", result)
        self.assertIn("min_compatible=2", result)
        self.assertIn("[compatible]", result)
        self.assertNotIn("INCOMPATIBLE", result)

    def test_incompatible_versions_flagged_as_hard_error_not_buried(self):
        """The negative case this item exists for: a real mismatch must be
        impossible to miss by skimming, not just another key in the dump."""
        pico_status = {
            "phase": "idle", "percent": 0, "last_error": "",
            "protocol_version_known": True,
            "protocol_version": 1,
            "protocol_min_compatible": 3,
            "protocol_compatible": False,
        }
        with self._patch(pico_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("INCOMPATIBLE", result)
        self.assertIn("INCOMPATIBLE PROTOCOL VERSION:", result)

    def test_unknown_version_reported_as_unknown_not_silently_dropped(self):
        pico_status = {"phase": "idle", "percent": 0, "last_error": "",
                        "protocol_version_known": False}
        with self._patch(pico_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("protocol_version=unknown", result)


if __name__ == "__main__":
    unittest.main()
