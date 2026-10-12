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


_OMIT_PRESERVE_RE = re.compile(r"\belse[^{]*\{[^{}]*?current_z->")


def _classify(text: str) -> "tuple[set[str], set[str]]":
    """(preserved, defaulted). A key is a candidate when a field_present /
    field_nonblank call follows its snprintf within 4 lines. It is
    omit-PRESERVED only if its block (up to the next key snprintf) has an
    else branch assigning from current_z->; otherwise omit-DEFAULTED.
    coupling_c%u is normalised to coupling_c; gkey sub-keys are ignored."""
    lines = text.splitlines()
    starts = [n for n, line in enumerate(lines) if _KEY_RE.search(line)]
    preserved, defaulted = set(), set()
    for idx, n in enumerate(starts):
        end = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
        if not _PRESERVE_RE.search("\n".join(lines[n + 1:n + 5])):
            continue
        name = _KEY_RE.search(lines[n]).group(1)
        name = "coupling_c" if name.startswith("coupling_c") else name
        block = "\n".join(lines[n + 1:end])
        (preserved if _OMIT_PRESERVE_RE.search(block) else defaulted).add(name)
    return preserved, defaulted


def firmware_omit_preserved_keys(text: str) -> "set[str]":
    return _classify(text)[0]


def firmware_omit_defaulted_keys(text: str) -> "set[str]":
    return _classify(text)[1]


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

    def test_defaulted_keys_match_firmware_and_are_never_stripped(self):
        text = _SRC.read_text(encoding="utf-8")
        self.assertEqual(firmware_omit_defaulted_keys(text), set(zhc.ZONE_OMIT_DEFAULTED_KEYS))
        self.assertEqual(set(zhc.ZONE_OMIT_DEFAULTED_KEYS) & set(zhc.ZONE_OMIT_PRESERVED_FLOAT_KEYS), set())
        self.assertEqual(set(zhc.ZONE_OMIT_DEFAULTED_KEYS) & set(zhc.ZONE_OMIT_PRESERVED_NONFLOAT_KEYS), set())
        for k in tuple(zhc.ZONE_OMIT_DEFAULTED_KEYS) + tuple(zhc.ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS):
            self.assertFalse(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)
            body = f"z1_{k}=3&z1_kp=1"
            self.assertIn(f"z1_{k}", zhc.strip_omit_preserved(body), k)

    def test_omit_branch_must_assign_from_current(self):
        def blk(name, assign):
            return [f'snprintf(key, sizeof(key), "z%u_{name}", i);', "{",
                    " if (zones_config_json_field_present(body, key)) { x = 1; }",
                    " else {", f" {assign}", " }", "}"]
        src = chr(10).join(blk("foo", "z->foo = 0.0f;") + blk("bar", "z->bar = current_z->bar;"))
        self.assertEqual(firmware_omit_preserved_keys(src), {"bar"})
        self.assertEqual(firmware_omit_defaulted_keys(src), {"foo"})

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
