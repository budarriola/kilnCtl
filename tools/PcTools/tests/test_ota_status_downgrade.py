#!/usr/bin/env python3
"""Unit tests for ota_status()'s downgrade reporting (mcp_server_ota.py) --
UPDATE_PROTOCOL.md's "a downgrade is allowed but logged as such" bullet.

App/drivers/persist/ota_record.c's ota_version_compare() computes
is_downgrade/version_compare_known at accept time (best-effort dotted/dashed
numeric parse of esp_app_desc_t.version strings) and ota_http_esp.c's
GET /api/ota/esp/status surfaces them as `downgrade_known`/`is_downgrade`
inside `last_update`. ota_http_client.get_esp_status() passes the JSON
through unmodified (plain json.loads) -- this file only exercises
mcp_server_ota.ota_status()'s own rendering of those two fields, same
"mocked ota_http_client, no real socket" shape as
test_ota_status_protocol_version.py.

Run with: python -m pytest tools/PcTools/tests/test_ota_status_downgrade.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota as mo  # noqa: E402


class OtaStatusDowngradeTest(unittest.TestCase):
    def _patch(self, esp_status: dict, pico_status: dict | None = None):
        pico_status = pico_status if pico_status is not None else {
            "phase": "idle", "percent": 0, "last_error": "",
            "protocol_version_known": False,
        }
        return unittest.mock.patch.multiple(
            mo.ota_http,
            get_pico_status=unittest.mock.Mock(return_value=pico_status),
            get_esp_status=unittest.mock.Mock(return_value=esp_status),
        )

    def test_downgrade_reported_plainly(self):
        esp_status = {
            "phase": "idle", "percent": 0,
            "last_update": {
                "processor": "esp", "version_before": "v2.5-10-gabc1234",
                "version_after": "v2.3-1-gdef5678", "success": True,
                "reason": "ok", "uptime_s": 12, "image_sha256": "",
                "downgrade_known": True, "is_downgrade": True,
            },
        }
        with self._patch(esp_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("[DOWNGRADE]", result)

    def test_upgrade_not_reported_as_downgrade(self):
        """Negative case: a real upgrade must never be mislabeled."""
        esp_status = {
            "phase": "idle", "percent": 0,
            "last_update": {
                "processor": "esp", "version_before": "v2.3-1-gdef5678",
                "version_after": "v2.5-10-gabc1234", "success": True,
                "reason": "ok", "uptime_s": 12, "image_sha256": "",
                "downgrade_known": True, "is_downgrade": False,
            },
        }
        with self._patch(esp_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("[upgrade/same]", result)
        self.assertNotIn("[DOWNGRADE]", result)

    def test_unknown_comparison_reported_as_unknown_not_a_silent_no(self):
        """version_compare_known=false must read as 'unknown', never as a
        false 'not a downgrade' answer -- same class of bug this codebase's
        own 'a sentinel is safe for comparers, unsafe for storers' note
        warns about."""
        esp_status = {
            "phase": "idle", "percent": 0,
            "last_update": {
                "processor": "esp", "version_before": "", "version_after": "",
                "success": True, "reason": "ok", "uptime_s": 12,
                "image_sha256": "", "downgrade_known": False, "is_downgrade": False,
            },
        }
        with self._patch(esp_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("[unknown (version strings not comparable)]", result)
        self.assertNotIn("[DOWNGRADE]", result)

    def test_no_last_update_unaffected(self):
        esp_status = {"phase": "idle", "percent": 0, "last_update": None}
        with self._patch(esp_status):
            result = mo.ota_status(host="10.0.0.5")
        self.assertIn("none (no ESP update has run this boot's NVS lifetime)", result)


if __name__ == "__main__":
    unittest.main()
