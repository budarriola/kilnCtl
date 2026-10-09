#!/usr/bin/env python3
"""Regression guard for the "moved code, left its import behind" defect
class found after the devices.py / mcp_server.py split (commit 8ae2deb):
``devices_info.py``'s ``PinConfigEntry.label`` called ``pin_function_label()``,
a name that was never imported into (or defined in) that module -- it still
lived only in the ``devices.py`` re-export surface it was split out of. Any
call to ``info_checks()`` -- or anything else touching ``.label``/``.abbrev``
-- raised ``NameError``.

The existing suite did not catch it because nothing in it exercised that
code path. Two checks close that gap:

1. ``test_module_imports_cleanly`` imports every module in the ``kilnctrl``
   package plus the top-level ``selfcheck*.py`` scripts, so an outright
   ImportError/NameError at *import* time cannot land silently.

2. ``test_no_undefined_names`` runs pyflakes (as a library, via
   ``pyflakes.checker.Checker``) over those same files and fails on any
   real "undefined name" finding -- including one hiding behind a
   ``from .x import *`` star import, which pyflakes itself only downgrades
   to "may be undefined, or defined from star imports" and does not fail
   on by default (that is exactly the shape the known bug took, since every
   ``devices_*.py``/``mcp_server_*.py`` submodule does ``from .protocol
   import *``). This second check is the one that actually matters here:
   import-time checking alone would NOT have caught the devices_info.py bug,
   because the bad name sat inside a ``@property`` body that plain
   ``import kilnctrl.devices_info`` never executes.

Verified (see the task history / commit message for this file) by
temporarily reverting the devices_info.py fix and re-running
``test_no_undefined_names`` for that one file: it failed with
``undefined name(s) ['pin_function_label', 'pin_function_abbrev']``, then
passed again once the fix was restored.
"""
from __future__ import annotations

import ast
import importlib
import os
import pkgutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import pyflakes.messages as pf_messages  # noqa: E402
from pyflakes.checker import Checker  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent  # tools/PcTools
SRC = ROOT / "src"


def _kilnctrl_module_names() -> list[str]:
    import kilnctrl

    names = ["kilnctrl"]
    for info in pkgutil.walk_packages(kilnctrl.__path__, prefix="kilnctrl."):
        names.append(info.name)
    return sorted(names)


def _selfcheck_module_names() -> list[str]:
    return sorted(p.stem for p in ROOT.glob("selfcheck*.py"))


@pytest.mark.parametrize("modname", _kilnctrl_module_names())
def test_kilnctrl_module_imports_cleanly(modname: str) -> None:
    """Every kilnctrl submodule must import without ImportError/NameError."""
    importlib.import_module(modname)


@pytest.mark.parametrize("modname", _selfcheck_module_names())
def test_selfcheck_module_imports_cleanly(modname: str) -> None:
    """Every selfcheck*.py split module must import cleanly too."""
    importlib.import_module(modname)


def _target_files() -> list[Path]:
    files = sorted((SRC / "kilnctrl").glob("*.py"))
    files += sorted(ROOT.glob("selfcheck*.py"))
    return files


def _resolve_star_import_sources(tree: ast.Module) -> dict[str, "set[str] | None"]:
    """Map each `from .mod import *` in this file to the set of names its
    target module actually exports, by importing it for real -- so a name
    pyflakes only flags as "may be undefined, or defined from star imports"
    can be resolved precisely instead of being ignored wholesale. A value of
    None means the source module itself failed to import (treated as
    contributing no names, i.e. anything attributed to it stays flagged)."""
    resolved: dict[str, "set[str] | None"] = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.ImportFrom):
            continue
        if not any(a.name == "*" for a in node.names):
            continue
        if not node.module:
            continue
        # Every star import in this tree is `from .something import *`
        # (relative, within the kilnctrl package) -- confirmed by grep
        # across devices*.py/mcp_server*.py; nothing else in the split uses
        # `import *`. Resolve relative to kilnctrl regardless of level, so a
        # differently-leveled star import fails loudly here rather than
        # being silently skipped.
        modname = f"kilnctrl.{node.module}"
        try:
            mod = importlib.import_module(modname)
        except Exception:
            resolved[modname] = None
            continue
        resolved[modname] = set(dir(mod))
    return resolved


def _undefined_names_in(path: Path) -> list[str]:
    src = path.read_text(encoding="utf-8")
    tree = ast.parse(src, filename=str(path))
    checker = Checker(tree, filename=str(path))
    star_sources: "dict[str, set] | None" = None
    real: list[str] = []
    for msg in checker.messages:
        if isinstance(msg, pf_messages.UndefinedName):
            real.append(msg.message_args[0])
        elif isinstance(msg, pf_messages.ImportStarUsage):
            name = msg.message_args[0]
            if star_sources is None:
                star_sources = _resolve_star_import_sources(tree)
            if not any(names and name in names for names in star_sources.values()):
                real.append(name)
    return real


# Pre-existing, split-unrelated findings live here so the guard stays scoped
# to the "moved code, left an import behind" class of defect it exists to
# catch (see the module docstring). All of these predate commit 8ae2deb --
# confirmed by checking the pre-split monolithic devices.py/gui.py at
# 8ae2deb^, where the same patterns already existed -- and are distinct bugs,
# not this guard's job to fix:
#
#   - gui_about.py / gui_display.py "exc": a lambda closing over an
#     `except ... as exc:` name, which Python deletes at the end of the
#     except block, so the deferred (self.post()) lambda raises NameError if
#     it is ever actually invoked.
#   - devices_profiles.py / devices_wifi_uart.py "Optional": a dataclass
#     field/parameter annotated with the *string* `"Optional[int]"` in a file
#     that never imports `typing.Optional`. Dataclasses never evaluate
#     string annotations at runtime by default, so this is dormant rather
#     than a live NameError -- but it would break under
#     `typing.get_type_hints()` or similar introspection.
_KNOWN_PRE_EXISTING = {
    (SRC / "kilnctrl" / "gui_about.py", "exc"),
    (SRC / "kilnctrl" / "gui_display.py", "exc"),
    (SRC / "kilnctrl" / "devices_profiles.py", "Optional"),
    (SRC / "kilnctrl" / "devices_wifi_uart.py", "Optional"),
}


@pytest.mark.parametrize("path", _target_files(), ids=lambda p: p.name)
def test_no_undefined_names(path: Path) -> None:
    """No module in the split may reference a name it never imported or
    defined -- the devices_info.py/pin_function_label class of bug."""
    found = _undefined_names_in(path)
    found = [n for n in found if (path, n) not in _KNOWN_PRE_EXISTING]
    assert not found, f"{path}: undefined name(s) {found}"
