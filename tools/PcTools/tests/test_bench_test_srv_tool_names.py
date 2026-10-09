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


def _is_srv_expr(value: ast.AST) -> bool:
    """``srv`` or a ``_srv(...)`` call (the cases_* helper returning the server)."""
    if isinstance(value, ast.Name):
        return value.id == "srv"
    return (isinstance(value, ast.Call) and isinstance(value.func, ast.Name)
            and value.func.id == "_srv")


def called_srv_names(source: str) -> set[str]:
    names = set()
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Attribute) and _is_srv_expr(node.value):
            names.add(node.attr)
    return names


def called_module_attrs(source: str) -> set[tuple[str, str]]:
    """(module, attr) for every ``<alias>.<attr>`` where alias came from
    ``from .. import mcp_server_<x> [as alias]`` (e.g. ``mcp_server_ota._ota_resolve_host``)."""
    tree = ast.parse(source)
    aliases = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.level >= 1:
            for a in node.names:
                if a.name.startswith("mcp_server_"):
                    aliases[a.asname or a.name] = a.name
    out = set()
    for node in ast.walk(tree):
        if (isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name)
                and node.value.id in aliases):
            out.add((aliases[node.value.id], node.attr))
    return out


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

    def test_every_module_qualified_attribute_exists(self):
        import importlib
        missing = {}
        for fn in sorted(os.listdir(PKG)):
            if not fn.endswith(".py"):
                continue
            with open(os.path.join(PKG, fn), encoding="utf-8") as f:
                for mod, attr in called_module_attrs(f.read()):
                    m = importlib.import_module(f"kilnctrl.{mod}")
                    if not hasattr(m, attr):
                        missing.setdefault(fn, []).append(f"{mod}.{attr}")
        self.assertEqual(missing, {}, "module-qualified attribute not exported")

    def test_scanner_sees_srv_call_chain_and_module_attrs(self):
        self.assertEqual(called_srv_names("_srv(ctx).no_such_tool_xyz()"), {"no_such_tool_xyz"})
        self.assertEqual(
            called_module_attrs("from .. import mcp_server_ota as o\no._nope()"),
            {("mcp_server_ota", "_nope")})
        found = set()
        for fn in os.listdir(PKG):
            if fn.endswith(".py"):
                with open(os.path.join(PKG, fn), encoding="utf-8") as f:
                    src = f.read()
                    found |= {("srv", n) for n in called_srv_names(src)}
                    found |= called_module_attrs(src)
        self.assertIn(("srv", "control_get_aux_outputs"), found)
        self.assertIn(("mcp_server_ota", "_ota_resolve_host"), found)

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
