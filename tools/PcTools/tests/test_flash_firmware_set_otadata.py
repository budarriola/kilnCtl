#!/usr/bin/env python3
"""flash_firmware(set_otadata_app=True) wiring: the OpenOCD command list
gains one otadata program_esp step (both sectors selecting ota_0) after the
app image and before the final reset; default False adds nothing.
Mocked OpenOCD only; no board."""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import test_flash_firmware_erase_partitions as _erase  # noqa: E402
from kilnctrl import mcp_server_flash as mf  # noqa: E402


class SetOtadataTest(unittest.TestCase):
    setUp = _erase.EraseFlashFirmwareEndToEndTest.setUp

    def _cmds(self):
        return [c.strip() for c in self.run_mock.call_args.args[2].split(";")]

    def test_default_has_no_otadata_step(self):
        mf.flash_firmware(confirm=True, kiln_fw_root=self.kiln_fw_root, verify=False)
        self.assertFalse(any("otadata" in c for c in self._cmds()))

    def test_true_appends_otadata_step_before_reset(self):
        captured = {}
        orig = mf.otadata_blob.build_otadata_image

        def spy(size, seq=1):
            img = orig(size, seq)
            captured["img"] = img
            return img

        self.addCleanup(setattr, mf.otadata_blob, "build_otadata_image", orig)
        mf.otadata_blob.build_otadata_image = spy
        result = mf.flash_firmware(confirm=True, kiln_fw_root=self.kiln_fw_root, verify=False, set_otadata_app=True)
        self.assertIn("flashed and verified OK", result)
        cmds = self._cmds()
        app_i = next(i for i, c in enumerate(cmds) if c.startswith("program_esp build/KilnCtrl.bin"))
        od_i = next(i for i, c in enumerate(cmds) if "otadata_app.bin" in c)
        self.assertLess(app_i, od_i)
        self.assertEqual(od_i, len(cmds) - 1)
        self.assertIn("0x200000", cmds[od_i])
        self.assertIn("verify", cmds[od_i])
        self.assertIn("reset exit", cmds[od_i])
        img = captured["img"]
        self.assertEqual(len(img), 0x2000)
        self.assertEqual(struct.unpack_from("<I", img, 0)[0], 1)
        self.assertEqual(img[:32], img[0x1000:0x1020])

    def test_missing_otadata_partition_refused_before_openocd(self):
        csv = os.path.join(self.kiln_fw_root, "partitions.csv")
        txt = open(csv, encoding="utf-8").read().replace("otadata,", "xotadata,")
        open(csv, "w", encoding="utf-8").write(txt)
        result = mf.flash_firmware(confirm=True, kiln_fw_root=self.kiln_fw_root, verify=False, set_otadata_app=True)
        self.assertTrue(result.startswith("error:"))
        self.run_mock.assert_not_called()



if __name__ == "__main__":
    unittest.main()
