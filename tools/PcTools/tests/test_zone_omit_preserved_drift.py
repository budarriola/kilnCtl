"""Drift test: zones_http_client's omit-preserved key lists must match what
firmware/KilnFW/App/drivers/http/zones_http_post_parse.c really treats as
omit-preserves-current (review BENCHFX2 LOW-1)."""
import re
import unittest
from pathlib import Path

from kilnctrl import zones_http_client as zhc

_SRC = (Path(__file__).resolve().parents[3]
        / "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c")
_KEY_RE = re.compile(r'snprintf\(\s*g?key,\s*sizeof\(g?key\),\s*"z%u_([a-z_0-9]+?)(?:%u)?"')
_PRESERVE_RE = re.compile(r"zones_config_json_field_present\(body, key\)|zones_http_field_nonblank\(body, key\)")


def firmware_omit_preserved_keys(text: str) -> "set[str]":
    """Zone POST keys whose snprintf is followed within 4 lines by a
    field_present / field_nonblank guard. coupling_c%u is normalised to
    coupling_c; settings_source's group sub-keys are ignored (gkey)."""
    lines = text.splitlines()
    out = set()
    for n, line in enumerate(lines):
        m = _KEY_RE.search(line)
        if not m:
            continue
        window = "\n".join(lines[n + 1:n + 5])
        if _PRESERVE_RE.search(window):
            name = m.group(1)
            out.add("coupling_c" if name.startswith("coupling_c") else name)
    return out


def python_omit_preserved_keys() -> "set[str]":
    return (set(zhc.ZONE_OMIT_PRESERVED_FLOAT_KEYS) | set(zhc.ZONE_OMIT_PRESERVED_NONFLOAT_KEYS)
            | {"coupling_c"})


class ZoneOmitPreservedDriftTest(unittest.TestCase):
    def test_firmware_source_present_and_parsed(self):
        self.assertTrue(_SRC.is_file(), _SRC)  # never skip: a moved file must fail loudly
        self.assertGreater(len(firmware_omit_preserved_keys(_SRC.read_text(encoding="utf-8"))), 25)

    def test_python_lists_match_firmware(self):
        fw = firmware_omit_preserved_keys(_SRC.read_text(encoding="utf-8"))
        py = python_omit_preserved_keys()
        self.assertEqual(fw - py, set(), "omit-preserved in firmware but unlisted in Python")
        self.assertEqual(py - fw, set(), "listed in Python but not omit-preserved in firmware")

    def test_regex_matches_exactly_the_float_keys(self):
        for k in zhc.ZONE_OMIT_PRESERVED_FLOAT_KEYS:
            self.assertTrue(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)
        self.assertTrue(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match("z2_coupling_c0"))
        for k in zhc.ZONE_OMIT_PRESERVED_NONFLOAT_KEYS:
            self.assertFalse(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)
        for k in ("kp", "ki", "kd", "maxtemp", "mintemp", "relay_mask", "cal", "ramp", "sanity",
                  "mode", "window", "minon", "minoff", "timingprofile", "name"):
            self.assertFalse(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)

    def test_guard_keys_stripped_from_body(self):
        body = "z0_runawaymargin=12.2&z0_xzone=5.0&z0_debounce=3&z0_kp=1&z0_zonetype=1"
        out = zhc.strip_omit_preserved(body)
        self.assertEqual(out, "z0_kp=1&z0_zonetype=1")
        kept = zhc.strip_omit_preserved(body, {"z0_runawaymargin"})
        self.assertIn("z0_runawaymargin", kept)


if __name__ == "__main__":
    unittest.main()
