#!/usr/bin/env python3
"""Mirror-drift guard for safety_cfg_http_client.unset_applicable_commissioning_params()
against firmware/KilnFW/App/drivers/http/readiness_http.h's
readiness_param_required_for_commissioning() -- the exclusion rule that
decides which safety_cfg_store params count toward GET /api/readiness's
"safety_commissioned" item ("N of M applicable safety parameters still have
no value").

Two independent copies of the same rule exist: the C switch in
readiness_http.h (feeds the wire), and the Python re-derivation in
safety_cfg_http_client.py (feeds safety_get_unset_commissioning_params()).
Nothing forces them to agree -- this is the repo's documented "reset one
side of a pair" / mirror-drift bug class (CLAUDE.md), the same class
firmware/KilnFW/App/test/readiness_ct_channel_map_mirror_drift_check.py
guards on the Pico-vs-ESP side of. This check guards the ESP-vs-PcTools
side instead: does the Python client's hardcoded id tuples and applicable()
branches still match the C source they were derived from.

Technique (extract-and-evaluate, same as the template check above, not
extract-and-diff -- the two sites have different shapes):
  1. Regex-extract the six READINESS_PARAM_ID_* #define values from
     readiness_http.h and compare them to safety_cfg_http_client.py's
     _CT_CHANNEL_MAP_IDS / _I_NORMAL_A_IDS tuples.
  2. Regex-extract each case group's `return <expr>;` from
     readiness_param_required_for_commissioning() in readiness_http.h,
     translate to a Python-evaluable boolean, and evaluate across the full
     (ct_installed, ct_topology) grid against
     unset_applicable_commissioning_params() fed synthetic
     GET /api/safety/commissioning responses.
  3. Assert readiness_http.c still hardcodes the 0x0109u/0x031Fu param ids
     and still defaults ct_installed_value = 1, ct_topology_value = 0 --
     the safe-direction defaults unset_applicable_commissioning_params()
     itself hardcodes and must keep matching.

Fails closed: any regex that stops matching its target is a test failure,
never a silent pass -- same contract as this repo's other
*_mirror_drift_check.py / *_drift_check.py scripts.

Run with: python -m pytest tools/PcTools/tests/test_readiness_client_mirror_drift.py -q
"""
from __future__ import annotations

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import safety_cfg_http_client as sc  # noqa: E402

_REPO_ROOT = __import__("pathlib").Path(__file__).resolve().parents[3]
_READINESS_H = _REPO_ROOT / "firmware" / "KilnFW" / "App" / "drivers" / "http" / "readiness_http.h"
_READINESS_C = _REPO_ROOT / "firmware" / "KilnFW" / "App" / "drivers" / "http" / "readiness_http.c"

_DEFINE_RE = re.compile(r"#define\s+READINESS_PARAM_ID_(\w+)\s+(0x[0-9A-Fa-f]+)u")

_CASE_GROUP_RE = re.compile(
    r"switch \(param_id\) \{\s*"
    r"case READINESS_PARAM_ID_CT_CHANNEL_MAP_0:\s*\n\s*case READINESS_PARAM_ID_CT_CHANNEL_MAP_1:\s*\n"
    r"\s*case READINESS_PARAM_ID_CT_CHANNEL_MAP_2:\s*\n\s*return ([^;]+);\s*\n"
    r"\s*case READINESS_PARAM_ID_I_NORMAL_A_0:\s*\n\s*case READINESS_PARAM_ID_I_NORMAL_A_1:\s*\n"
    r"\s*case READINESS_PARAM_ID_I_NORMAL_A_2:\s*\n\s*return ([^;]+);",
    re.DOTALL,
)


def _c_bool_to_py(expr: str) -> str:
    e = expr.strip()
    e = e.replace("ct_installed_value", "installed").replace("ct_topology_value", "topology")
    e = e.replace("!= 0u", "!= 0").replace("== 0u", "== 0")
    e = e.replace("&&", "and")
    allowed_tokens = ("installed", "topology", "!=", "==", "and", "(", ")", "0", " ")
    stripped = e
    for tok in allowed_tokens:
        stripped = stripped.replace(tok, "")
    if stripped != "":
        raise ValueError(f"unrecognized token(s) {stripped!r} in {expr!r}")
    return e


def _synthetic_response(installed, topology, extra_ids=()):
    """A GET /api/safety/commissioning-shaped dict with ct_installed/
    ct_topology set as given, plus one representative unset param from each
    of the two gated groups (and, optionally, extra always-applicable
    params), all reported unset -- enough to probe applicable()."""
    params = [
        {"id": sc._CT_INSTALLED_ID, "name": "ct_installed", "type": "u8", "set": True, "value": installed},
        {"id": sc._CT_TOPOLOGY_ID, "name": "ct_topology", "type": "u8", "set": True, "value": topology},
        {"id": sc._CT_CHANNEL_MAP_IDS[0], "name": "ct_channel_map[0]", "type": "u8", "set": False},
        {"id": sc._I_NORMAL_A_IDS[0], "name": "i_normal_a[0]", "type": "f32", "set": False},
    ]
    for pid in extra_ids:
        params.append({"id": pid, "name": f"extra_{pid:04x}", "type": "u8", "set": False})
    return {"unset_reporting_reliable": True, "params": params}


class ReadinessClientIdMirrorTest(unittest.TestCase):
    """Part 1: the six wire ids agree."""

    def setUp(self):
        self.assertTrue(_READINESS_H.is_file(), f"missing {_READINESS_H}")
        self.text = _READINESS_H.read_text(encoding="utf-8")
        defines = dict(_DEFINE_RE.findall(self.text))
        self.assertTrue(defines, "READINESS_PARAM_ID_* regex found nothing -- "
                                  "update _DEFINE_RE rather than letting this pass vacuously")
        self.defines = {k: int(v, 16) for k, v in defines.items()}

    def test_all_six_defines_found(self):
        expected_names = {
            "CT_CHANNEL_MAP_0", "CT_CHANNEL_MAP_1", "CT_CHANNEL_MAP_2",
            "I_NORMAL_A_0", "I_NORMAL_A_1", "I_NORMAL_A_2",
        }
        missing = expected_names - set(self.defines)
        self.assertFalse(missing, f"expected READINESS_PARAM_ID_* defines not found: {missing}")

    def test_ct_channel_map_ids_match_client(self):
        c_ids = tuple(self.defines[f"CT_CHANNEL_MAP_{i}"] for i in range(3))
        self.assertEqual(c_ids, sc._CT_CHANNEL_MAP_IDS)

    def test_i_normal_a_ids_match_client(self):
        c_ids = tuple(self.defines[f"I_NORMAL_A_{i}"] for i in range(3))
        self.assertEqual(c_ids, sc._I_NORMAL_A_IDS)


class ReadinessClientExpressionMirrorTest(unittest.TestCase):
    """Part 2: the applicability rule itself agrees across the full grid."""

    def setUp(self):
        self.assertTrue(_READINESS_H.is_file(), f"missing {_READINESS_H}")
        text = _READINESS_H.read_text(encoding="utf-8")
        match = _CASE_GROUP_RE.search(text)
        self.assertIsNotNone(
            match,
            "could not locate readiness_param_required_for_commissioning()'s case groups -- "
            "update _CASE_GROUP_RE rather than letting this pass vacuously",
        )
        self.ct_channel_map_expr = _c_bool_to_py(match.group(1))
        self.i_normal_a_expr = _c_bool_to_py(match.group(2))

    def _c_side_applicable(self, param_id, installed, topology):
        env = {"installed": installed, "topology": topology}
        if param_id in sc._CT_CHANNEL_MAP_IDS:
            return bool(eval(self.ct_channel_map_expr, {"__builtins__": {}}, env))  # noqa: S307 -- vetted charset in _c_bool_to_py
        if param_id in sc._I_NORMAL_A_IDS:
            return bool(eval(self.i_normal_a_expr, {"__builtins__": {}}, env))  # noqa: S307
        return True

    def test_full_grid_agrees(self):
        mismatches = []
        for installed in (0, 1):
            for topology in (0, 1):
                resp = _synthetic_response(installed, topology)
                py_unset_ids = {p["id"] for p in sc.unset_applicable_commissioning_params(resp)}
                for pid in sc._CT_CHANNEL_MAP_IDS[:1] + sc._I_NORMAL_A_IDS[:1]:
                    c_applicable = self._c_side_applicable(pid, installed, topology)
                    py_applicable = pid in py_unset_ids  # every probed param is reported unset
                    if c_applicable != py_applicable:
                        mismatches.append((pid, installed, topology, c_applicable, py_applicable))
        self.assertEqual(
            mismatches, [],
            f"readiness_http.h and safety_cfg_http_client.py disagree on applicability: {mismatches}",
        )

    def test_negative_a_deliberately_wrong_client_rule_is_caught(self):
        """Negative test: prove the grid comparison actually distinguishes
        agreement from disagreement, by evaluating a deliberately WRONG
        stand-in applicable() (ct_channel_map required unconditionally,
        ignoring topology) against the real C-side expression and
        confirming at least one grid cell disagrees."""

        def wrong_applicable(param_id, installed, topology):
            if param_id in sc._CT_CHANNEL_MAP_IDS:
                return installed != 0  # wrong: drops the topology==per_zone condition
            if param_id in sc._I_NORMAL_A_IDS:
                return installed != 0
            return True

        mismatches = []
        for installed in (0, 1):
            for topology in (0, 1):
                for pid in (sc._CT_CHANNEL_MAP_IDS[0], sc._I_NORMAL_A_IDS[0]):
                    c_applicable = self._c_side_applicable(pid, installed, topology)
                    wrong = wrong_applicable(pid, installed, topology)
                    if c_applicable != wrong:
                        mismatches.append((pid, installed, topology))
        self.assertTrue(
            mismatches,
            "the deliberately-wrong stand-in rule agreed with the real C expression on every "
            "grid cell -- this test's own grid is not sensitive enough to catch real drift",
        )


class ReadinessCHardcodedDefaultsTest(unittest.TestCase):
    """Part 3: readiness_http.c's own hardcoded ids and safe-direction
    defaults, which unset_applicable_commissioning_params() mirrors, have
    not moved."""

    def setUp(self):
        self.assertTrue(_READINESS_C.is_file(), f"missing {_READINESS_C}")
        self.text = _READINESS_C.read_text(encoding="utf-8")

    def test_hardcoded_param_ids_present(self):
        self.assertIn("0x0109u", self.text, "ct_installed param id 0x0109u not found in readiness_http.c")
        self.assertIn("0x031Fu", self.text, "ct_topology param id 0x031Fu not found in readiness_http.c")

    def test_default_ct_installed_value_is_one(self):
        self.assertRegex(
            self.text, r"ct_installed_value\s*=\s*1\s*;",
            "ct_installed_value's safe-direction default no longer reads `= 1` in readiness_http.c",
        )

    def test_default_ct_topology_value_is_zero(self):
        self.assertRegex(
            self.text, r"ct_topology_value\s*=\s*0\s*;",
            "ct_topology_value's safe-direction default no longer reads `= 0` in readiness_http.c",
        )


if __name__ == "__main__":
    unittest.main()
