#!/usr/bin/env python3
"""Unit tests for flash_firmware()'s post-flash verification
(mcp_server_flash.py: _verify_flash_landed() and the verify= wiring).

History: flash_firmware() writes only the ESP32's `factory` partition and
never touches `otadata`. If an OTA ever pointed the boot target at
ota_0/ota_1, the bootloader keeps booting that OLD image forever -- every
later flash_firmware() reports "flashed and verified OK" (OpenOCD's own
byte-compare during the write) while the board silently keeps running old
code. This is the recurring "my change vanished" debugging-session drain
CLAUDE.md documents. _verify_flash_landed() closes that gap by independently
asking the RUNNING firmware (over HTTP) what partition it's booting and what
build it reports, and comparing both against what was just flashed.

All against MOCKED partition_http_client.get_partitions,
capability_preflight.get_board_info, and a synthetic on-disk .bin (built the
same way test_esp_app_desc.py does) -- no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_flash_firmware_verify.py -q
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_flash as mf  # noqa: E402
from kilnctrl import esp_app_desc  # noqa: E402
from kilnctrl import partition_http_client  # noqa: E402
from kilnctrl import capability_preflight  # noqa: E402


def _pad(s: str, width: int) -> bytes:
    b = s.encode("utf-8")
    return b + b"\x00" * (width - len(b))


def _write_bin(time_s: str = "20:13:41", date_s: str = "Sep  3 2026") -> str:
    header = b"\xff" * esp_app_desc.APP_DESC_OFFSET
    desc = struct.pack("<I", esp_app_desc.ESP_APP_DESC_MAGIC_WORD)
    desc += struct.pack("<I", 1)
    desc += b"\x00" * 8
    desc += _pad("v1", 32)
    desc += _pad("KilnCtrl", 32)
    desc += _pad(time_s, 16)
    desc += _pad(date_s, 16)
    desc += b"\x00" * (32 + 32 + 80)
    f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    f.write(header + desc)
    f.close()
    return f.name


class VerifyFlashLandedTest(unittest.TestCase):
    def setUp(self):
        self.bin_path = _write_bin(time_s="20:13:41", date_s="Sep  3 2026")
        self.addCleanup(os.unlink, self.bin_path)

    def test_running_factory_and_matching_build_succeeds_silently(self):
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "factory", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  3 2026 20:13:41"),
        ):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertEqual(result, "")

    def test_running_ota_0_fails_loud_and_names_it(self):
        """The core bug this whole change exists to catch: bootloader still
        booting a stale OTA slot after a `factory`-only flash."""
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "ota_0", "partitions": []},
        ):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed("192.168.1.50", self.bin_path)
        msg = str(ctx.exception)
        self.assertIn("ota_0", msg)
        self.assertIn("ota_rollback_esp", msg)

    def test_build_timestamp_mismatch_fails_loud_with_both_values(self):
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "factory", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  1 2026 08:00:00"),
        ):
            with self.assertRaises(RuntimeError) as ctx:
                mf._verify_flash_landed("192.168.1.50", self.bin_path)
        msg = str(ctx.exception)
        self.assertIn("Sep  1 2026 08:00:00", msg)
        self.assertIn("Sep  3 2026 20:13:41", msg)

    def test_http_unreachable_warns_does_not_raise(self):
        """verify=True default's HTTP-unreachable case must WARN, never
        hard-fail -- distinct from an actual wrong-partition/build finding.
        Speeds through all poll attempts by mocking time.sleep."""
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            side_effect=partition_http_client.PartitionHttpError("unreachable: timed out"),
        ), unittest.mock.patch.object(mf.time, "sleep"):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertIn("WARNING", result)
        self.assertIn("skipped", result)

    def test_status_unreachable_after_partition_ok_warns_not_raises(self):
        with unittest.mock.patch.object(
            partition_http_client, "get_partitions",
            return_value={"running": "factory", "partitions": []},
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=False, error="timed out"),
        ):
            result = mf._verify_flash_landed("192.168.1.50", self.bin_path)
        self.assertIn("WARNING", result)
        self.assertIn("factory", result)

    def test_unparseable_bin_warns_does_not_raise(self):
        bad_bin = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        bad_bin.write(b"not a real image")
        bad_bin.close()
        self.addCleanup(os.unlink, bad_bin.name)
        result = mf._verify_flash_landed("192.168.1.50", bad_bin.name)
        self.assertIn("WARNING", result)


class FlashFirmwareVerifyWiringTest(unittest.TestCase):
    """End-to-end through flash_firmware() itself: proves verify=True/False
    actually gate _verify_flash_landed, and that a verification failure
    surfaces as the tool's returned error string (flash_firmware() never
    raises out to its caller -- _tool()'s wrapper would mangle the message,
    so _post_flash catches RuntimeError itself)."""

    def setUp(self):
        self._openocd_patch = unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe")
        self._openocd_patch.start()
        self.addCleanup(self._openocd_patch.stop)

        self._kill_patch = unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value="")
        self._kill_patch.start()
        self.addCleanup(self._kill_patch.stop)

        self._run_patch = unittest.mock.patch.object(mf, "_run_openocd", return_value=(True, "verified"))
        self._run_patch.start()
        self.addCleanup(self._run_patch.stop)

        stale_ok = unittest.mock.Mock(stale=False, reason="")
        self._stale_patch = unittest.mock.patch.object(mf.stale_check, "check_kilnfw_stale", return_value=stale_ok)
        self._stale_patch.start()
        self.addCleanup(self._stale_patch.stop)

        self._isfile_patch = unittest.mock.patch.object(mf.os.path, "isfile", return_value=True)
        self._isfile_patch.start()
        self.addCleanup(self._isfile_patch.stop)

    def test_verify_false_skips_verification_entirely(self):
        with unittest.mock.patch.object(mf, "_verify_flash_landed") as verify_mock:
            result = mf.flash_firmware(verify=False)
        verify_mock.assert_not_called()
        self.assertIn("flashed and verified OK", result)

    def test_verify_true_failure_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mf, "_verify_flash_landed",
            side_effect=RuntimeError("running partition 'ota_0', not 'factory' -- call ota_rollback_esp()"),
        ):
            result = mf.flash_firmware(verify=True)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("ota_0", result)
        self.assertIn("ota_rollback_esp", result)

    def test_verify_true_warning_is_appended_not_a_failure(self):
        with unittest.mock.patch.object(mf, "_verify_flash_landed", return_value="WARNING: board unreachable"):
            result = mf.flash_firmware(verify=True)
        self.assertFalse(result.startswith("error:"))
        self.assertIn("WARNING: board unreachable", result)

class DefaultHostResolutionTest(unittest.TestCase):
    """REGRESSION: every test above passes an EXPLICIT host, so none of them
    exercised the default (host=None) path -- the one every documented
    `flash_firmware()` call actually takes. That path used to resolve
    straight to PARTITION_AP_DEFAULT_HOST (192.168.4.1), an address a board
    on home Wi-Fi does not answer, so verification always timed out into the
    "could not reach the board" WARNING and never once ran on the failure it
    exists to catch. It must use the package-wide _ota_resolve_host() (STA IP
    first), the same resolution debug_check_partition_table() uses."""

    def setUp(self):
        self.bin_path = _write_bin()
        self.addCleanup(os.unlink, self.bin_path)

    def test_default_host_uses_sta_ip_not_the_ap_fallback(self):
        from kilnctrl import mcp_server_ota

        seen = []

        def fake_get_partitions(host, timeout=None):
            seen.append(host)
            return {"running": "factory", "partitions": []}

        with unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="192.168.1.77"
        ), unittest.mock.patch.object(
            partition_http_client, "get_partitions", side_effect=fake_get_partitions
        ), unittest.mock.patch.object(
            capability_preflight, "get_board_info",
            return_value=capability_preflight.BoardInfo(reachable=True, fw_build="Sep  3 2026 20:13:41"),
        ):
            result = mf._verify_flash_landed(None, self.bin_path)
        self.assertEqual(result, "")
        self.assertEqual(seen, ["192.168.1.77"])
        self.assertNotIn(partition_http_client.PARTITION_AP_DEFAULT_HOST, seen)


if __name__ == "__main__":
    unittest.main()
