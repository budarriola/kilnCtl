"""Shared helper for App/test scripts that read a specific
firmware/KilnFW/App/drivers file by name.

Several of this directory's drift-guard / fingerprint checks
(approach_rate_cap_mirror_drift_check.py, fuzzy_gain_mirror_drift_check.py,
ramp_lock_decision_mirror_drift_check.py, ramp_stepping_gate_mirror_drift_check.py,
frame_a_offset_drift_check.py, power_diag_flag_mirror_drift_check.py,
wire_protocol_fingerprint_check.py) used to build a flat
``repo_root / "firmware/KilnFW/App/drivers/<name>"`` path by hand.
firmware/KilnFW/App/drivers/ is being split into layer subdirectories
(``drivers/<layer>/<name>``); a flat path silently stops resolving the
moment its file moves. ``resolve_driver_file`` replaces the flat path with
a recursive basename lookup under drivers/, and fails loudly (not silently,
not vacuously) if the name is missing or ambiguous -- the same contract as
tools/PcTools/tests/_drivers_layout.py's Python helper and the check
scripts' PowerShell ``Resolve-DriverFile`` function.
"""
from __future__ import annotations

from pathlib import Path


class DriverFileError(Exception):
    """Raised when a named driver file cannot be resolved unambiguously."""


def resolve_driver_file(repo_root: Path, basename: str, *, drivers_dir: Path | None = None) -> Path:
    """Find ``basename`` anywhere under ``<repo_root>/firmware/KilnFW/App/drivers``
    (or under ``drivers_dir`` if given -- e.g. a simulated split-layout smoke
    test rooted somewhere else entirely), plus -- as of the WP1 hardware-
    abstraction split (aa8581c moved uart_protocol.{c,h} out from under
    drivers/owners/ to firmware/hwAbstraction/esp/uart/) -- under
    ``firmware/hwAbstraction/esp/`` and ``firmware/hwAbstraction/pico/`` as
    secondary roots. A caller that passes ``drivers_dir`` explicitly (the
    simulated-layout smoke tests) opts out of the hwAbstraction roots too,
    same as it always opted out of the real drivers/ root -- it is testing
    a specific tree, not "wherever this basename happens to live today".
    Raises DriverFileError if the name is missing or ambiguous *across all
    searched roots combined*, never returns a nonexistent/wrong path
    silently.
    """
    if drivers_dir is not None:
        roots = [drivers_dir]
    else:
        root = Path(repo_root)
        roots = [
            root / "firmware" / "KilnFW" / "App" / "drivers",
            root / "firmware" / "hwAbstraction" / "esp",
            root / "firmware" / "hwAbstraction" / "pico",
        ]

    existing_roots = [r for r in roots if r.is_dir()]
    if not existing_roots:
        raise DriverFileError(
            f"expected drivers directory not found: {roots[0]} -- has it moved or been renamed?"
        )

    matches = []
    for r in existing_roots:
        matches.extend(r.rglob(basename))
    matches = sorted(set(matches))

    if not matches:
        raise DriverFileError(
            f"expected file {basename!r} not found anywhere under {existing_roots} -- "
            "has it moved or been renamed?"
        )
    if len(matches) > 1:
        raise DriverFileError(
            f"{basename!r} matched more than one file under {existing_roots}: {matches} -- "
            "cannot tell which one is the real file."
        )
    return matches[0]
