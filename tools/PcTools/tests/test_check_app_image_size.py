"""Tests for tools/check_app_image_size.py -- the 4 MiB `app` size gate
(docs/GITHUB_RELEASE_UPDATE_PLAN.md section 3, WP2). No board, no build."""
from __future__ import annotations

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

_REPO = Path(__file__).resolve().parents[3]
_spec = importlib.util.spec_from_file_location("check_app_image_size", _REPO / "tools" / "check_app_image_size.py")
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)

_CSV = (
    "otadata, data, ota, 0x200000, 0x2000,\n"
    "app, app, ota_0, 0x210000, {app},\n"
    "stage, data, undefined, 0x610000, 0x400000,\n"
    "recovery, app, factory, 0xA10000, 0x1E0000,\n"
)


class AppImageSizeGate(unittest.TestCase):
    def _run(self, app_row: str, image_size: int | None, ceiling: int = gate.APP_SIZE_CEILING) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            csv = os.path.join(tmp, "partitions.csv")
            Path(csv).write_text(_CSV.format(app=app_row))
            binp = os.path.join(tmp, "KilnCtrl.bin")
            if image_size is not None:
                with open(binp, "wb") as f:
                    f.truncate(image_size)
            return gate.run(csv, binp, "app", ceiling)

    def test_fits(self) -> None:
        self.assertEqual(self._run("0x400000", 2_600_000), 0)

    def test_exactly_full_passes(self) -> None:
        self.assertEqual(self._run("0x400000", 0x400000), 0)

    def test_one_byte_over_fails(self) -> None:
        self.assertEqual(self._run("0x400000", 0x400001), 1)

    def test_row_grown_past_ceiling_fails(self) -> None:
        self.assertEqual(self._run("0x800000", 2_600_000), 1)

    def test_shrunken_row_is_the_bound(self) -> None:
        self.assertEqual(self._run("0x200000", 0x200001), 1)

    def test_missing_image_skips(self) -> None:
        self.assertEqual(self._run("0x400000", None), 3)

    def test_real_table_pins_4mib(self) -> None:
        from kilnctrl.partition_table import parse_partitions_csv

        rows = {e.name: e for e in parse_partitions_csv(str(_REPO / "firmware" / "KilnFW" / "partitions.csv"))}
        self.assertEqual((rows["app"].offset, rows["app"].size), (0x210000, 0x400000))
        self.assertEqual((rows["stage"].offset, rows["stage"].size), (0x610000, 0x400000))
        self.assertEqual(rows["app"].offset + rows["app"].size, rows["stage"].offset)
        self.assertEqual(rows["stage"].offset + rows["stage"].size, rows["recovery"].offset)


if __name__ == "__main__":
    unittest.main()
