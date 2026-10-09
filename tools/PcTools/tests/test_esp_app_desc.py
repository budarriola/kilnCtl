#!/usr/bin/env python3
"""Unit tests for kilnctrl.esp_app_desc -- parses esp_app_desc_t out of a
raw ESP-IDF app image (no live board, no real .bin, a synthetic blob built
in-test). Backs flash_firmware()'s post-flash build-timestamp verification.

Run with: python -m pytest tools/PcTools/tests/test_esp_app_desc.py -q
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import esp_app_desc  # noqa: E402


def _pad(s: str, width: int) -> bytes:
    b = s.encode("utf-8")
    assert len(b) < width, "test fixture string too long for field"
    return b + b"\x00" * (width - len(b))


def _build_image(
    *,
    magic: int = esp_app_desc.ESP_APP_DESC_MAGIC_WORD,
    version: str = "v1.2.3",
    project_name: str = "KilnCtrl",
    time_s: str = "20:13:41",
    date_s: str = "Sep  3 2026",
    header_size: int = esp_app_desc.APP_DESC_OFFSET,
    truncate_to: int = None,
) -> bytes:
    """Builds a synthetic app image: `header_size` bytes of arbitrary
    header padding, then an esp_app_desc_t at that offset with the given
    field values."""
    header = b"\xff" * header_size
    desc = struct.pack("<I", magic)  # magic_word
    desc += struct.pack("<I", 1)  # secure_version
    desc += b"\x00" * 8  # reserv1
    desc += _pad(version, 32)
    desc += _pad(project_name, 32)
    desc += _pad(time_s, 16)
    desc += _pad(date_s, 16)
    # idf_ver / sha256 / reserv2 aren't read by the parser -- pad with a
    # plausible amount of trailing bytes so a real image's shape is exercised.
    desc += b"\x00" * (32 + 32 + 80)
    image = header + desc
    if truncate_to is not None:
        image = image[:truncate_to]
    return image


class ParseAppDescTest(unittest.TestCase):
    def test_parses_time_and_date_from_valid_image(self):
        image = _build_image(time_s="20:13:41", date_s="Sep  3 2026")
        desc = esp_app_desc.parse_app_desc(image)
        self.assertEqual(desc.time, "20:13:41")
        self.assertEqual(desc.date, "Sep  3 2026")
        self.assertEqual(desc.build_timestamp, "Sep  3 2026 20:13:41")

    def test_parses_version_and_project_name(self):
        image = _build_image(version="v4.5.6", project_name="KilnCtrl")
        desc = esp_app_desc.parse_app_desc(image)
        self.assertEqual(desc.version, "v4.5.6")
        self.assertEqual(desc.project_name, "KilnCtrl")

    def test_bad_magic_word_raises_clear_error_not_crash(self):
        """NEGATIVE TEST target: a corrupt/non-app image must be refused with
        a clear AppDescError, not silently parsed as garbage and not an
        unhandled struct/index exception."""
        image = _build_image(magic=0xDEADBEEF)
        with self.assertRaises(esp_app_desc.AppDescError) as ctx:
            esp_app_desc.parse_app_desc(image)
        self.assertIn("magic", str(ctx.exception).lower())

    def test_truncated_image_raises_clear_error_not_crash(self):
        image = _build_image(truncate_to=esp_app_desc.APP_DESC_OFFSET + 4)
        with self.assertRaises(esp_app_desc.AppDescError) as ctx:
            esp_app_desc.parse_app_desc(image)
        self.assertIn("short", str(ctx.exception).lower())

    def test_empty_image_raises_clear_error(self):
        with self.assertRaises(esp_app_desc.AppDescError):
            esp_app_desc.parse_app_desc(b"")


class ParseAppDescFileTest(unittest.TestCase):
    def test_reads_and_parses_a_real_file(self):
        image = _build_image(time_s="09:00:00", date_s="Jan  1 2027")
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(image)
            path = f.name
        try:
            desc = esp_app_desc.parse_app_desc_file(path)
            self.assertEqual(desc.build_timestamp, "Jan  1 2027 09:00:00")
        finally:
            os.unlink(path)


class BuildTimestampsMatchTest(unittest.TestCase):
    def test_exact_match(self):
        desc = esp_app_desc.parse_app_desc(_build_image(time_s="20:13:41", date_s="Sep  3 2026"))
        self.assertTrue(esp_app_desc.build_timestamps_match(desc, "Sep  3 2026 20:13:41"))

    def test_whitespace_normalized_match(self):
        """ESP-IDF's __DATE__ pads single-digit days with an extra space;
        this must still count as a match once whitespace-normalized."""
        desc = esp_app_desc.parse_app_desc(_build_image(time_s="20:13:41", date_s="Sep 13 2026"))
        self.assertTrue(esp_app_desc.build_timestamps_match(desc, "Sep 13 2026  20:13:41"))

    def test_mismatch_is_false(self):
        desc = esp_app_desc.parse_app_desc(_build_image(time_s="20:13:41", date_s="Sep  3 2026"))
        self.assertFalse(esp_app_desc.build_timestamps_match(desc, "Sep  4 2026 20:13:41"))

    def test_none_fw_build_is_false(self):
        desc = esp_app_desc.parse_app_desc(_build_image())
        self.assertFalse(esp_app_desc.build_timestamps_match(desc, None))


if __name__ == "__main__":
    unittest.main()
