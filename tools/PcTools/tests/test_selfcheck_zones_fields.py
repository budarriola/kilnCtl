#!/usr/bin/env python3
"""Tests for selfcheck_zones_fields.py (B7 ROADMAP.md M15 item).

The extraction logic is exercised against synthetic C snippets that mimic
zones_http_handlers.c's shape (multi-fragment APPEND() calls split across
lines, a C comment interrupting a fragment run, a nested object, arrays of
objects) rather than the real firmware file, so this test does not drift
with unrelated firmware edits and pins down exactly what the extractor is
supposed to handle.

Run with: python -m pytest tools/PcTools/tests/test_selfcheck_zones_fields.py
"""
from __future__ import annotations

import os
import pathlib
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from selfcheck_zones_fields import (
    _extract_get_top_level_keys,
    _extract_post_top_level_keys,
    _function_body,
)

_SYNTHETIC_GET = r"""
esp_err_t zones_get_handler(httpd_req_t *req)
{
    APPEND("{\"thermo_count\":%u,\"relay_count\":%u,"
           "\"safety_tc_type\":%u,"
           /* a multi-line block comment sitting between two adjacent
            * string-literal fragments of the SAME APPEND() call -- must
            * not truncate the capture, same shape as the real
            * relay_zone_owned_mask comment in zones_http_handlers.c. */
           "\"relay_zone_owned_mask\":%u,"
           "\"safety_wiring\":{\"link_up\":%s,\"tc_fault\":%u},"
           "\"relay_names\":[",
           a, b, c, d, e, f);
    for (int r = 0; r < N; r++) {
        APPEND("%s\"%s\"", r == 0 ? "" : ",", names[r]);
    }
    APPEND("],\"timing_profiles\":[");
    for (int p = 0; p < N; p++) {
        APPEND("%s{\"index\":%u,\"name\":\"%s\"}", p == 0 ? "" : ",", p, name);
    }
    APPEND("],\"zones\":[");
    for (int i = 0; i < N; i++) {
        APPEND("%s{\"index\":%u,\"pid_kp\":%.4f}", i == 0 ? "" : ",", i, kp);
    }
    APPEND("]}");
    return ESP_OK;
}

esp_err_t zones_post_handler(httpd_req_t *req)
{
    if (!zones_config_json_parse_u8_field(body, "thermo_count", 0, N, &tmp.thermo_count)) {
        return ESP_FAIL;
    }
    if (!zones_config_json_parse_u8_field(body, "relay_count", 0, N, &tmp.relay_count)) {
        return ESP_FAIL;
    }
    {
        int len = http_form_find_field(body, "safety_tc_type", val, sizeof(val));
    }
    /* per-zone fields build their key dynamically -- never a literal, so
     * this must NOT show up as a top-level POST field. */
    char key[16];
    snprintf(key, sizeof(key), "z%u_pid_kp", i);
    zones_config_json_parse_float_field(body, key, 0.0f, 100.0f, &z->pid_kp);
    return ESP_OK;
}

esp_err_t zones_pid_post_handler(httpd_req_t *req)
{
    /* a different handler's literal fields -- must NOT leak into
     * zones_post_handler's extracted set. */
    zones_config_json_parse_u8_field(body, "zone", 0, N - 1, &zone_index);
    zones_config_json_parse_float_field(body, "kp", 0.0f, MAX, &kp);
    return ESP_OK;
}
"""


class FunctionBodySlicingTests(unittest.TestCase):
    def test_slices_only_the_named_function(self):
        body = _function_body(_SYNTHETIC_GET, "zones_post_handler", pathlib.Path("<synthetic>"))
        self.assertIn('"thermo_count"', body)
        self.assertNotIn('"zone"', body)  # that's zones_pid_post_handler's field
        self.assertNotIn("zones_get_handler", body)

    def test_missing_function_raises(self):
        with self.assertRaises(AssertionError):
            _function_body(_SYNTHETIC_GET, "no_such_handler", pathlib.Path("<synthetic>"))


class ExtractGetTopLevelKeysTests(unittest.TestCase):
    def test_finds_scalar_and_structural_top_level_keys(self):
        keys = _extract_get_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertEqual(
            keys,
            {
                "thermo_count", "relay_count", "safety_tc_type",
                "relay_zone_owned_mask", "safety_wiring",
                "relay_names", "timing_profiles", "zones",
            },
        )

    def test_nested_object_keys_excluded(self):
        keys = _extract_get_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertNotIn("link_up", keys)
        self.assertNotIn("tc_fault", keys)

    def test_per_element_array_keys_excluded(self):
        keys = _extract_get_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertNotIn("index", keys)
        self.assertNotIn("name", keys)
        self.assertNotIn("pid_kp", keys)

    def test_comment_between_fragments_does_not_truncate_capture(self):
        # If the comment DID truncate the capture, relay_zone_owned_mask
        # (and everything after it) would be missing.
        keys = _extract_get_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertIn("relay_zone_owned_mask", keys)
        self.assertIn("zones", keys)


class ExtractPostTopLevelKeysTests(unittest.TestCase):
    def test_finds_literal_top_level_fields(self):
        keys = _extract_post_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertEqual(keys, {"thermo_count", "relay_count", "safety_tc_type"})

    def test_dynamic_per_zone_key_excluded(self):
        keys = _extract_post_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertNotIn("pid_kp", keys)

    def test_other_handlers_fields_excluded(self):
        keys = _extract_post_top_level_keys(_SYNTHETIC_GET, pathlib.Path("<synthetic>"))
        self.assertNotIn("zone", keys)
        self.assertNotIn("kp", keys)


class RealFirmwareSmokeTest(unittest.TestCase):
    """Not a synthetic-snippet test -- a light smoke check that the real
    firmware file still parses to a non-trivial key set, so a firmware
    refactor that breaks the extractor's assumptions is caught here too,
    not just in selfcheck.py's own run."""

    def test_real_firmware_file_extracts_known_keys(self):
        import pathlib

        drivers_dir = (
            pathlib.Path(__file__).resolve().parents[3]
            / "firmware" / "KilnFW" / "App" / "drivers"
        )
        # zones_http_handlers.c was split into zones_http_get.c /
        # zones_http_post.c (and possibly further siblings later) -- glob
        # for the family instead of hardcoding one filename so the next
        # split doesn't break this test again.
        paths = sorted(drivers_dir.glob("zones_http_*.c"))
        if not paths:
            self.fail(
                f"no zones_http_*.c source found under {drivers_dir} -- "
                "the firmware file was renamed/split/moved again and this "
                "smoke test's glob no longer matches anything; update the "
                "glob rather than letting this report green with zero "
                "coverage"
            )
        text = "\n".join(p.read_text(encoding="utf-8") for p in paths)
        get_keys = _extract_get_top_level_keys(text, paths[0])
        post_keys = _extract_post_top_level_keys(text, paths[0])
        self.assertIn("thermo_count", get_keys)
        self.assertIn("safety_wiring", get_keys)
        self.assertIn("zones", get_keys)
        self.assertIn("thermo_count", post_keys)
        self.assertNotIn("zone", post_keys)  # that's zones_pid_post_handler's


if __name__ == "__main__":
    unittest.main()
