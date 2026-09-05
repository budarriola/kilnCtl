#!/usr/bin/env python3
"""Regression guard for kilnctrl.protocol.AUTOTUNE_RULES against the
firmware sources it hand-mirrors: firmware/KilnFW/App/drivers/
pid_autotune.h's autotune_rule_t enum (the rule set that exists) and
firmware/KilnFW/App/drivers/dashboard_http.c's autotune_start_post_handler()
(which method accepts which rule names -- the closest thing to an
authoritative "supported rule names" string table, since it is what the
board's own POST /api/autotune/start parses).

Mirrors tools/PcTools/tests/test_uart_version_independence.py's technique:
regex the C source directly rather than trusting the Python side to have
stayed in sync by hand. Without this test, AUTOTUNE_RULES going stale (a
fifth autotune_rule_t member added, or dashboard_http.c's accepted string
set changed) reproduces the exact defect kilnctrl.mcp_server.autotune_start()
was fixed for: the PC tool refuses a rule the firmware actually implements.

Run with: python -m pytest tools/PcTools/tests/test_autotune_rules_drift_guard.py -q
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

from kilnctrl.protocol import AUTOTUNE_RULES

from _drivers_layout import resolve_driver_file

_REPO_ROOT = Path(__file__).resolve().parents[3]
_PID_AUTOTUNE_H = resolve_driver_file(_REPO_ROOT, "pid_autotune.h")
# dashboard_http.c (2026-09-04, ROADMAP.md M15's 1500-line rule) was split
# into dashboard_http.c plus siblings; autotune_start_post_handler() and its
# "rule must be ..." refusal messages -- the exact text this guard reads --
# moved into dashboard_autotune_http.c.
_DASHBOARD_HTTP_C = resolve_driver_file(_REPO_ROOT, "dashboard_autotune_http.c")

#: The ONLY place a firmware C enum member name is translated to this
#: module's short rule-name spelling. If pid_autotune.h grows a fifth
#: AUTOTUNE_RULE_* member, parse_autotune_rule_enum() below will surface it
#: as an enum name with no entry here, and the parity test fails loudly
#: instead of the mismatch going unnoticed.
_ENUM_NAME_TO_SHORT = {
    "AUTOTUNE_RULE_SIMC": "simc",
    "AUTOTUNE_RULE_ZIEGLER_NICHOLS": "zn",
    "AUTOTUNE_RULE_TYREUS_LUYBEN": "tl",
    "AUTOTUNE_RULE_COHEN_COON": "cohen-coon",
}


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return text


def parse_autotune_rule_enum(header_text: str) -> "list[str]":
    """Every AUTOTUNE_RULE_* token inside the `typedef enum { ... }
    autotune_rule_t;` block of pid_autotune.h -- order-preserving, so a
    reordering (which would change the wire *values*, not just the set)
    is visible to a caller that cares, though this module's parity check
    only cares about the set of names."""
    code = _strip_comments(header_text)
    match = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*autotune_rule_t\s*;", code, flags=re.DOTALL)
    assert match is not None, "no `typedef enum { ... } autotune_rule_t;` found in pid_autotune.h"
    return re.findall(r"\bAUTOTUNE_RULE_[A-Z_]+\b", match.group(1))


def parse_dashboard_http_rule_names(source_text: str) -> "dict[str, set[str]]":
    """The rule-name strings dashboard_http.c's autotune_start_post_handler()
    actually accepts, keyed by method ("relay"/"step"), recovered from its
    own `rule must be "X" or "Y"` refusal messages (~lines 2027-2030 for
    relay, 2059-2061 for step) -- those messages are themselves the
    authoritative list of what strcmp() checks against on each path, so a
    firmware change to the accepted set has to touch the same string this
    regex reads."""
    code = _strip_comments(source_text)
    messages = re.findall(r'rule must be \\"([a-z-]+)\\" or \\"([a-z-]+)\\"', code)
    assert len(messages) == 2, (
        f"expected exactly 2 'rule must be ... or ...' refusal messages in dashboard_http.c "
        f"(one relay, one step), found {len(messages)} -- has autotune_start_post_handler() "
        "been restructured?"
    )
    relay_names, step_names = messages
    return {"relay": set(relay_names), "step": set(step_names)}


class AutotuneRulesDriftGuardTests(unittest.TestCase):
    def setUp(self):
        self.header_text = _PID_AUTOTUNE_H.read_text(encoding="utf-8")
        self.dashboard_text = _DASHBOARD_HTTP_C.read_text(encoding="utf-8")

    def test_firmware_sources_exist(self):
        self.assertTrue(_PID_AUTOTUNE_H.is_file(), _PID_AUTOTUNE_H)
        self.assertTrue(_DASHBOARD_HTTP_C.is_file(), _DASHBOARD_HTTP_C)

    def test_every_enum_member_has_a_short_name_mapping(self):
        """THE drift trigger: a fifth AUTOTUNE_RULE_* member with no entry
        in _ENUM_NAME_TO_SHORT fails here first, before the parity checks
        below even run -- see test_fake_fifth_rule_is_caught for proof."""
        enum_names = parse_autotune_rule_enum(self.header_text)
        unmapped = [n for n in enum_names if n not in _ENUM_NAME_TO_SHORT]
        self.assertEqual(
            unmapped, [],
            f"pid_autotune.h defines AUTOTUNE_RULE_* member(s) {unmapped} with no entry in "
            "this test's _ENUM_NAME_TO_SHORT -- add the short name mapping here AND to "
            "protocol.AUTOTUNE_RULES, or the PC tool will refuse a rule the firmware supports")

    def test_autotune_rules_keys_match_firmware_enum(self):
        enum_names = parse_autotune_rule_enum(self.header_text)
        expected_short_names = {_ENUM_NAME_TO_SHORT[n] for n in enum_names if n in _ENUM_NAME_TO_SHORT}
        self.assertEqual(set(AUTOTUNE_RULES), expected_short_names)

    def test_autotune_rules_methods_match_dashboard_http_parsing(self):
        by_method = parse_dashboard_http_rule_names(self.dashboard_text)
        for method, fw_names in by_method.items():
            pc_names = {name for name, info in AUTOTUNE_RULES.items() if method in info["methods"]}
            self.assertEqual(
                pc_names, fw_names,
                f"AUTOTUNE_RULES' {method} rule set {pc_names} does not match "
                f"dashboard_http.c's accepted {method} rule set {fw_names}")

    # ---- NEGATIVE TESTS: prove the parser actually catches drift ----

    def test_fake_fifth_rule_is_caught(self):
        """NEGATIVE TEST: inject a fake fifth enum member into a COPY of the
        header text (the real file is never touched) and confirm
        parse_autotune_rule_enum() reports it as unmapped, reproducing
        exactly what would make test_every_enum_member_has_a_short_name_mapping
        fail on real drift."""
        fake_header = self.header_text.replace(
            "AUTOTUNE_RULE_COHEN_COON,",
            "AUTOTUNE_RULE_COHEN_COON,\n    AUTOTUNE_RULE_MADE_UP_FOR_THIS_TEST,",
        )
        self.assertNotEqual(fake_header, self.header_text, "fixture replace() found nothing to inject")
        enum_names = parse_autotune_rule_enum(fake_header)
        unmapped = [n for n in enum_names if n not in _ENUM_NAME_TO_SHORT]
        self.assertEqual(unmapped, ["AUTOTUNE_RULE_MADE_UP_FOR_THIS_TEST"])

    def test_fake_dashboard_rule_name_changes_are_caught(self):
        """NEGATIVE TEST: if dashboard_http.c's step-path refusal message
        changed to accept a new rule name (simulated on a COPY of the
        source), parse_dashboard_http_rule_names() must report the new set,
        which would then fail test_autotune_rules_methods_match_dashboard_http_parsing
        against the unchanged AUTOTUNE_RULES."""
        fake_source = self.dashboard_text.replace(
            'rule must be \\"simc\\" or \\"cohen-coon\\"',
            'rule must be \\"simc\\" or \\"made-up-rule\\"',
        )
        self.assertNotEqual(fake_source, self.dashboard_text, "fixture replace() found nothing to inject")
        by_method = parse_dashboard_http_rule_names(fake_source)
        self.assertEqual(by_method["step"], {"simc", "made-up-rule"})
        self.assertNotEqual(by_method["step"], {name for name, info in AUTOTUNE_RULES.items()
                                                  if "step" in info["methods"]})


if __name__ == "__main__":
    unittest.main()
