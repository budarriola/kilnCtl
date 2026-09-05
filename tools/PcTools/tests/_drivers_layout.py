"""Shared helper for tests that read a specific firmware/KilnFW/App/drivers
file by name.

Several of this suite's drift-guard tests (test_autotune_rules_drift_guard.py,
test_autotune_wire_layout.py, test_cone_table.py, test_ramp_assist.py,
test_uart_version_independence.py, test_zones_page_relay_accept_enable.py)
read one specific driver source/header by hand-built flat path
(``.../App/drivers/<name>``) to regex or diff it against the Python side.
firmware/KilnFW/App/drivers/ is about to be split into layer subdirectories
(``drivers/<layer>/<name>``); a flat path silently stops resolving the moment
its file moves, and every one of those tests would then either raise a
generic FileNotFoundError with no hint of why, or (worse, for a glob) just
find nothing and report a misleadingly empty result.

``resolve_driver_file`` replaces the flat path with a recursive basename
lookup under drivers/, and fails loudly (not silently) if the name is not
found or is ambiguous -- mirroring the PowerShell checks' Resolve-DriverFile
helper (tools/check_heartbeat_contract.ps1 et al.).
"""
from __future__ import annotations

from pathlib import Path


def resolve_driver_file(repo_root: Path, basename: str, *, drivers_dir: Path | None = None) -> Path:
    """Find ``basename`` anywhere under ``<repo_root>/firmware/KilnFW/App/drivers``
    (or under ``drivers_dir`` if given, e.g. for a simulated split-layout
    smoke test). Raises AssertionError if it is missing or ambiguous.
    """
    base = drivers_dir if drivers_dir is not None else (
        Path(repo_root) / "firmware" / "KilnFW" / "App" / "drivers"
    )
    matches = sorted(base.rglob(basename))
    assert matches, (
        f"expected file {basename!r} not found anywhere under {base} -- "
        "has it moved or been renamed?"
    )
    assert len(matches) == 1, (
        f"{basename!r} matched more than one file under {base}: {matches} -- "
        "cannot tell which one is the real file."
    )
    return matches[0]
