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
    test rooted somewhere else entirely). Raises DriverFileError if it is
    missing or ambiguous, never returns a nonexistent/wrong path silently.
    """
    base = drivers_dir if drivers_dir is not None else (
        Path(repo_root) / "firmware" / "KilnFW" / "App" / "drivers"
    )
    if not base.is_dir():
        raise DriverFileError(
            f"expected drivers directory not found: {base} -- has it moved or been renamed?"
        )
    matches = sorted(base.rglob(basename))
    if not matches:
        raise DriverFileError(
            f"expected file {basename!r} not found anywhere under {base} -- "
            "has it moved or been renamed?"
        )
    if len(matches) > 1:
        raise DriverFileError(
            f"{basename!r} matched more than one file under {base}: {matches} -- "
            "cannot tell which one is the real file."
        )
    return matches[0]
