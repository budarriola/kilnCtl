"""Guard (BENCH_TEST_SYSTEM_PLAN section 8): no bench_test case calls an
``srv.<tool>(...)`` that the real ``kilnctrl.mcp_server`` module does not
export. Fakes in unit tests would otherwise hide a renamed/removed tool until
a bench run."""
from __future__ import annotations

import ast
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

PKG = os.path.join(os.path.dirname(__file__), "..", "src", "kilnctrl", "bench_test")


def called_srv_names(source: str) -> set[str]:
    names = set()
    for node in ast.walk(ast.parse(source)):
        if (isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name)
                and node.value.id == "srv"):
            names.add(node.attr)
    return names


class SrvToolNamesTest(unittest.TestCase):
    def test_every_srv_attribute_exists_on_real_server(self):
        from kilnctrl import mcp_server
        missing = {}
        for fn in sorted(os.listdir(PKG)):
            if not fn.endswith(".py"):
                continue
            with open(os.path.join(PKG, fn), encoding="utf-8") as f:
                for n in called_srv_names(f.read()):
                    if not hasattr(mcp_server, n):
                        missing.setdefault(fn, []).append(n)
        self.assertEqual(missing, {}, "srv.<name> not exported by mcp_server")

    def test_negative_scanner_sees_bogus_name(self):
        self.assertEqual(called_srv_names("srv.no_such_tool_xyz()"), {"no_such_tool_xyz"})
        from kilnctrl import mcp_server
        self.assertFalse(hasattr(mcp_server, "no_such_tool_xyz"))

    def test_scanner_finds_real_usage(self):
        found = set()
        for fn in os.listdir(PKG):
            if fn.endswith(".py"):
                with open(os.path.join(PKG, fn), encoding="utf-8") as f:
                    found |= called_srv_names(f.read())
        self.assertIn("safety_get_status", found)


if __name__ == "__main__":
    unittest.main()
