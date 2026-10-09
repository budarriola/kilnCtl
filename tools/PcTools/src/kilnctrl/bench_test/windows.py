"""Window probes (docs/BENCH_TEST_WEB_JUDGES_PLAN.md section 2 rule 6).

An observer case declares ``CaseSpec.window_probe = (window_name, fn)``. The
runner calls :func:`register_probes` before the case loop; a host case calls
:func:`fire_window` at the moment the window is open. Results land in
``ctx["_probe_results"][window][case_id]`` (a list per fire for the
append windows, a single dict otherwise). A probe that raises is stored as
``{"error": repr}`` and never changes the host's verdict. The window key is
created on the first fire even with no probes, so an observer can tell
"host never opened the window" (key absent) from "opened".
"""
from __future__ import annotations

from typing import Any, Optional

#: Windows that fire many times; their per-case result is a list of samples.
APPEND_WINDOWS = frozenset({"hp01_tick", "hp04_states"})


def register_probes(ctx: dict, requested, get_case) -> None:
    probes: dict = {}
    for cid in requested:
        wp = get_case(cid).window_probe
        if wp is not None:
            name, fn = wp
            probes.setdefault(name, []).append((cid, fn))
    ctx["_window_probes"] = probes
    ctx["_probe_results"] = {}
    ctx["_windows_fired"] = set()


def fire_window(ctx: dict, name: str, *, once: bool = False) -> None:
    """Run every registered probe for ``name``. ``once`` fires at most once
    per run. Never raises."""
    try:
        fired = ctx.setdefault("_windows_fired", set())
        if once:
            if name in fired:
                return
            fired.add(name)
        results = ctx.setdefault("_probe_results", {}).setdefault(name, {})
        for cid, fn in ctx.get("_window_probes", {}).get(name, []):
            try:
                value: Any = fn(ctx)
            except Exception as exc:  # noqa: BLE001
                value = {"error": repr(exc)}
            if name in APPEND_WINDOWS:
                results.setdefault(cid, []).append(value)
            else:
                results[cid] = value
    except Exception:  # noqa: BLE001 - a probe must never affect the host
        pass
