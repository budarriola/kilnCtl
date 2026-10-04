#!/usr/bin/env python3
"""Import-order regression: every ``kilnctrl.mcp_server_*`` submodule must be
importable FIRST (before the ``kilnctrl.mcp_server`` aggregate) without leaving
the aggregate missing that submodule's public names.

Background: the aggregate star-re-exports each submodule, and each submodule
needs the aggregate's ``mcp``/``_tool`` while it loads. When that dependency
pointed back at ``mcp_server`` itself, importing a submodule first re-entered
the aggregate mid-initialisation and its ``import *`` copied nothing, silently
dropping the submodule's tools from ``kilnctrl.mcp_server`` for the rest of the
process (test-order pollution). ``mcp_server_core.py`` breaks the cycle.

Each submodule is checked in a fresh subprocess so one import cannot mask
another. Run with: python -m pytest tools/PcTools/tests/test_mcp_server_import_order.py -q
"""
from __future__ import annotations

import ast
import os
import subprocess
import sys
import unittest
from concurrent.futures import ThreadPoolExecutor

_SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "src"))
_PKG = os.path.join(_SRC, "kilnctrl")


def _submodules() -> list[str]:
    return sorted(
        f[:-3] for f in os.listdir(_PKG)
        if f.startswith("mcp_server_") and f.endswith(".py")
    )


def _public_names(sub: str) -> list[str]:
    """Names ``from sub import *`` would export, derived from the source."""
    with open(os.path.join(_PKG, sub + ".py"), encoding="utf-8") as fh:
        tree = ast.parse(fh.read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "__all__" for t in node.targets
        ):
            return list(ast.literal_eval(node.value))
    names: list[str] = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names.append(node.name)
    return [n for n in names if not n.startswith("_")]


def _check(sub: str) -> tuple[str, int, str]:
    names = _public_names(sub)
    code = (
        f"import kilnctrl.{sub}; import kilnctrl.mcp_server as m; import sys; "
        f"missing=[n for n in {names!r} if not hasattr(m, n)]; "
        "print(missing); sys.exit(0 if not missing else 1)"
    )
    env = dict(os.environ, PYTHONPATH=_SRC, PYTHONDONTWRITEBYTECODE="1")
    proc = subprocess.run([sys.executable, "-c", code], env=env,
                          capture_output=True, text=True, timeout=300)
    return sub, proc.returncode, (proc.stdout + proc.stderr)[-600:]


class ImportOrderTests(unittest.TestCase):
    def test_every_submodule_importable_first(self):
        subs = _submodules()
        self.assertGreater(len(subs), 30)
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(_check, subs))
        failures = [r for r in results if r[1] != 0]
        print(f"\nimport-order: {len(results) - len(failures)}/{len(results)} submodules OK")
        self.assertEqual(
            failures, [],
            f"{len(failures)} of {len(results)} submodules lose names when imported before the aggregate: "
            + ", ".join(f[0] for f in failures),
        )


if __name__ == "__main__":
    unittest.main()
