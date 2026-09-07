"""check_mcp_facade_coverage.py -- every registered MCP tool on kilnctrl and
kicad must be reachable through that server's search facade (kiln_find /
kicad_find), and every taxonomy entry must still name a real tool.

WHY THIS EXISTS. `safety_get_diag` was registered as a kilnctrl MCP tool
(mcp_server_safety.py) but was absent from mcp_facade.py's taxonomy entirely
-- no GROUP_OVERRIDES entry, no KEYWORDS entry, and its "safety_" prefix
group existed but a per-tool keyword entry did not, so a caller asking
`kiln_find` for "boot reason" or "watchdog" (real words a caller would use for
that tool) never found it. The same 2026-09 pass found four more kilnctrl
tools (three `coupled_ident_*`, one `plant_sim_*`) whose naming prefix wasn't
in GROUP_PREFIXES at all, so `derive_group` silently fell through to a naive
`name.split("_", 1)[0]` guess instead of a deliberate group -- and one
registered "tool" (`_reject_kiln_fw_build_path`) that turned out to be an
internal helper accidentally decorated with `@_srv._tool()`, which this check
would otherwise have demanded a taxonomy entry for forever.

WHAT "COVERED" MEANS. A registered tool is covered if any of the following is
true, mirroring how mcpkit.registry.derive_group and the facade tables
actually work:
  - its name is a key in that server's facade GROUP_OVERRIDES, or
  - its name is a key in that server's facade KEYWORDS, or
  - its name starts with one of that server's facade GROUP_PREFIXES prefixes.
Any one of these means the tool has a *deliberate* entry point into the
search taxonomy, as opposed to riding on whatever `derive_group`'s fallback
guesses from the name alone.

WHAT THIS CATCHES, PER SERVER.
  - kilnctrl: tool names are `@_srv._tool()`-decorated functions across
    tools/PcTools/src/kilnctrl/mcp_server*.py, PLUS the tools attached
    through mcpkit.workbench.BUNDLES (build_kilnfw, build_saftyfw,
    build_saftyfw_host_tests, run_pctools_tests, run_repo_checks) -- these
    are registered via workbench.attach(), not the decorator, so a plain
    decorator scan alone would under-count and produce false "missing
    taxonomy" noise for tools that were never actually ungrouped.
  - kicad: tool names are the string keys of the big `self.tools = {...}`
    dict literal in kicad_mcp_server.py (built in __init__ before
    collapse_table() replaces it), plus whatever `_ipc_tools()` adds.

WHAT THIS DOES NOT CATCH. It says nothing about *keyword quality* -- a tool
with a technically-present-but-useless KEYWORDS tuple still passes. It also
does not run kiln_find/kicad_find itself or score real queries; it only
checks that a deliberate taxonomy entry point exists. Negative-tested by
temporarily deleting the `plant_sim_compare` KEYWORDS entry (which also has
no GROUP_PREFIXES/GROUP_OVERRIDES coverage) and confirming this script fails,
then restoring it.
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _fail(msg: str) -> None:
    print(f"check_mcp_facade_coverage: {msg}")


def _derive_group_covered(name: str, prefixes, overrides, keywords) -> bool:
    if name in overrides:
        return True
    if name in keywords:
        return True
    return any(name.startswith(p) for p, _ in prefixes)


def _load_facade_module(path: Path, module_name: str):
    """Import a facade module by path without needing it on sys.path
    permanently -- these are plain data modules (no heavy deps)."""
    import importlib.util

    spec = importlib.util.spec_from_file_location(module_name, path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# kilnctrl
# ---------------------------------------------------------------------------

def _kilnctrl_registered_names(kilnctrl_dir: Path) -> "set[str]":
    names: "set[str]" = set()
    for f in sorted(kilnctrl_dir.glob("mcp_server*.py")):
        lines = f.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            if line.strip() != "@_srv._tool()":
                continue
            j = i + 1
            while j < len(lines) and not lines[j].strip().startswith("def "):
                j += 1
            if j >= len(lines):
                continue
            m = re.match(r"def (\w+)\(", lines[j].strip())
            if m:
                names.add(m.group(1))

    # mcpkit.workbench.BUNDLES tools attached via workbench.attach(_tool, (...))
    # in mcp_server.py, not the @_srv._tool() decorator -- read which bundles
    # kilnctrl actually attaches, then union those BUNDLES keys in.
    server_py = kilnctrl_dir / "mcp_server.py"
    server_src = server_py.read_text(encoding="utf-8")
    m = re.search(r"workbench\.attach\(\s*_tool\s*,\s*\(([^)]*)\)\s*\)", server_src)
    if not m:
        raise RuntimeError(
            f"{server_py}: could not find 'workbench.attach(_tool, (...))' call -- "
            "has the registration pattern moved? Update _kilnctrl_registered_names."
        )
    bundle_names = [b.strip().strip("'\"") for b in m.group(1).split(",") if b.strip()]

    workbench_py = REPO_ROOT / "tools" / "PcTools" / "src" / "mcpkit" / "workbench.py"
    workbench_src = workbench_py.read_text(encoding="utf-8")
    workbench_ast = ast.parse(workbench_src, filename=str(workbench_py))
    bundles_dict = None
    for node in ast.walk(workbench_ast):
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "BUNDLES" for t in node.targets
        ):
            bundles_dict = node.value
            break
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) \
                and node.target.id == "BUNDLES" and isinstance(node.value, ast.Dict):
            bundles_dict = node.value
            break
    if bundles_dict is None or not isinstance(bundles_dict, ast.Dict):
        raise RuntimeError(f"{workbench_py}: could not find a 'BUNDLES = {{...}}' dict literal")

    for key_node, val_node in zip(bundles_dict.keys, bundles_dict.values):
        bundle_key = key_node.value if isinstance(key_node, ast.Constant) else None
        if bundle_key not in bundle_names or not isinstance(val_node, ast.Dict):
            continue
        for tool_key in val_node.keys:
            if isinstance(tool_key, ast.Constant):
                names.add(tool_key.value)

    return names


def _check_kilnctrl() -> bool:
    kilnctrl_dir = REPO_ROOT / "tools" / "PcTools" / "src" / "kilnctrl"
    facade = _load_facade_module(kilnctrl_dir / "mcp_facade.py", "kilnctrl_mcp_facade_check")

    registered = _kilnctrl_registered_names(kilnctrl_dir)
    taxonomy_names = set(facade.GROUP_OVERRIDES.keys()) | set(facade.KEYWORDS.keys())

    missing = sorted(
        n for n in registered
        if not _derive_group_covered(n, facade.GROUP_PREFIXES, facade.GROUP_OVERRIDES, facade.KEYWORDS)
    )
    stale = sorted(taxonomy_names - registered)
    dead_prefixes = sorted(
        p for p, _ in facade.GROUP_PREFIXES if not any(n.startswith(p) for n in registered)
    )

    ok = True
    if missing:
        ok = False
        _fail(f"kilnctrl: {len(missing)} registered tool(s) not covered by mcp_facade.py taxonomy:")
        for n in missing:
            print(f"    {n}")
        print(
            "    Fix: add a GROUP_PREFIXES entry for a shared prefix, or a KEYWORDS "
            "entry with the words a caller would actually type."
        )
    if stale:
        ok = False
        _fail(f"kilnctrl: {len(stale)} mcp_facade.py taxonomy entrie(s) name tools that no longer exist:")
        for n in stale:
            print(f"    {n}")
        print("    Fix: remove the stale GROUP_OVERRIDES/KEYWORDS entry, or rename it if the tool moved.")
    if dead_prefixes:
        ok = False
        _fail(f"kilnctrl: {len(dead_prefixes)} GROUP_PREFIXES entrie(s) match zero registered tools:")
        for p in dead_prefixes:
            print(f"    {p!r}")
        print("    Fix: remove the stale prefix, or rename it if the tool family it targeted moved.")

    if ok:
        print(f"check_mcp_facade_coverage: kilnctrl OK ({len(registered)} tools, all covered)")
    return ok


# ---------------------------------------------------------------------------
# kicad
# ---------------------------------------------------------------------------

def _kicad_registered_names(kicad_dir: Path) -> "set[str]":
    server_py = kicad_dir / "kicad_mcp_server.py"
    tree = ast.parse(server_py.read_text(encoding="utf-8"), filename=str(server_py))

    names: "set[str]" = set()

    def _collect_dict_str_keys(dict_node: ast.Dict, into: "set[str]") -> None:
        for key_node, val_node in zip(dict_node.keys, dict_node.values):
            if isinstance(key_node, ast.Constant) and isinstance(key_node.value, str) \
                    and isinstance(val_node, ast.Dict):
                into.add(key_node.value)

    found_main = False
    for node in ast.walk(tree):
        # self.tools: dict[...] = { ... }  (AnnAssign) inside __init__
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Attribute) \
                and node.target.attr == "tools" and isinstance(node.value, ast.Dict):
            _collect_dict_str_keys(node.value, names)
            found_main = True
        if isinstance(node, ast.FunctionDef) and node.name == "_ipc_tools":
            for stmt in node.body:
                if isinstance(stmt, ast.Return) and isinstance(stmt.value, ast.Dict):
                    _collect_dict_str_keys(stmt.value, names)

    if not found_main:
        raise RuntimeError(
            f"{server_py}: could not find 'self.tools: dict[...] = {{...}}' -- "
            "has the registration pattern moved? Update _kicad_registered_names."
        )
    return names


def _check_kicad() -> bool:
    kicad_dir = REPO_ROOT / "tools" / "mykicadMcp"
    facade = _load_facade_module(kicad_dir / "kicad_facade.py", "kicad_facade_check")

    registered = _kicad_registered_names(kicad_dir)
    taxonomy_names = set(facade.GROUP_OVERRIDES.keys()) | set(facade.KEYWORDS.keys())

    missing = sorted(
        n for n in registered
        if not _derive_group_covered(n, facade.GROUP_PREFIXES, facade.GROUP_OVERRIDES, facade.KEYWORDS)
    )
    stale = sorted(taxonomy_names - registered)
    dead_prefixes = sorted(
        p for p, _ in facade.GROUP_PREFIXES if not any(n.startswith(p) for n in registered)
    )

    ok = True
    if missing:
        ok = False
        _fail(f"kicad: {len(missing)} registered tool(s) not covered by kicad_facade.py taxonomy:")
        for n in missing:
            print(f"    {n}")
    if stale:
        ok = False
        _fail(f"kicad: {len(stale)} kicad_facade.py taxonomy entrie(s) name tools that no longer exist:")
        for n in stale:
            print(f"    {n}")
    if dead_prefixes:
        ok = False
        _fail(f"kicad: {len(dead_prefixes)} GROUP_PREFIXES entrie(s) match zero registered tools:")
        for p in dead_prefixes:
            print(f"    {p!r}")

    if ok:
        print(f"check_mcp_facade_coverage: kicad OK ({len(registered)} tools, all covered)")
    return ok


def main() -> int:
    ok_kilnctrl = _check_kilnctrl()
    ok_kicad = _check_kicad()
    return 0 if (ok_kilnctrl and ok_kicad) else 1


if __name__ == "__main__":
    sys.exit(main())
