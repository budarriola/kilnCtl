"""DRAM_PSRAM_PLAN.md section 4.3/7 measurement procedure: turns a live
``get_stack_margin()`` reading into a recorded baseline the plan can cite,
one capture at a time, so the plan's §3.1 table stops being a single
snapshot nobody can reproduce or extend.

Split deliberately into pure record-building/rendering functions (this
module, no I/O, unit-tested against fabricated ``StackMarginEntry``/
``FirmwareVersion`` objects -- never a real board) and the thin CLI wrapper
that does the actual UART round trip
(``tools/PcTools/scripts/capture_stack_margin_baseline.py``). That split is
what makes the procedure testable without hardware: everything except
"ask the device" lives here.

Why three load conditions (DRAM_PSRAM_PLAN.md section 4.3, "a baseline taken
on an idle board would understate the pressure"):

  * ``idle``       -- floor. No task is doing meaningful work, so any HWM
                       seen here is the SMALLEST plausible worst case; it
                       tells you nothing about whether a task is safe to
                       shrink, only what its absolute minimum footprint is.
  * ``mid_firing``  -- profile_executor's tick path, run_state/relay_cycles/
                       firing_stats writes, the safety-link watchdog path,
                       and (for kiln_io_owner/thermo_owner) the real
                       GPIO/SPI command traffic a firing generates are ONLY
                       exercised here. Section 3.1's existing profile_exec_wdt
                       and profile_executor numbers were both taken this way;
                       every 7.3 candidate's number needs the same load or it
                       is not comparable to them.
  * ``web_ui_open`` -- httpd_worker's own worst case (JSON serialization,
                       concurrent connections) only shows up with a client
                       actually attached; the plan's own soak definition
                       (section 5) requires two clients open throughout for
                       the same reason. Capture with at least one dashboard
                       tab open and polling, ideally two.

A task's TRUE worst case is the maximum HWM-shrinkage (i.e. minimum
hwm_bytes) across whichever of these conditions were actually captured --
see :func:`worst_case_across_conditions`. Capturing only one condition and
calling it "the" baseline is exactly the mistake section 4.3 warns against.

Why ``load`` exists (2026-09-04 finding, tools/check_stack_margin_baseline.py):
the first checked-in baseline was captured on a fully idle bench (no firing,
no autotune) but nothing in the file said so structurally -- only the
filename and the freeform ``notes`` string said "idle", and the ``condition``
argument above is exactly one typo away from lying (a caller can pass
"mid_firing" while the board sits idle and nothing catches it). ``load`` is
filled in by the CALLER from the board's OWN reported state
(ProfileExecStatus/AutotuneStatus, not an argument), so a capture's load
condition is what the board says it was doing, not what the operator meant
to be doing. See :class:`LoadSnapshot` and ``check_stack_margin_baseline.py``
for how a checked-in idle-only ``load`` is now treated.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Optional

from .devices_info import FirmwareVersion, StackMarginEntry
from .protocol import StackMarginLevel

#: The three conditions DRAM_PSRAM_PLAN.md section 4.3 requires. Order
#: matters only for display; a real baseline needs all three before any
#: candidate task's number is trustworthy (see this module's docstring).
LOAD_CONDITIONS: tuple[str, ...] = ("idle", "mid_firing", "web_ui_open")


@dataclass(frozen=True)
class LoadSnapshot:
    """What the board itself was doing at the moment a stack-margin reading
    was taken, read from ProfileExecStatus/AutotuneStatus -- never from a
    caller-supplied flag, which is one typo from lying (exactly what let the
    idle_2bcdc2d capture read as if the LOW tasks it saw might already be a
    worst case).

    ``firing_active``: profile_exec_status_t.state is RUNNING or PAUSED
        (system_uart_bridge's SYSTEM_CMD_FACTORY_RESET chain and
        telemetry_log's per-zone/AUTOTUNE_ENGINE_DONE snprintf chain are only
        reachable in the neighbourhood of a real firing or autotune run --
        see check_stack_margin_baseline.py's KNOWN_LOW_ALLOWLIST entries).
    ``autotune_active``: autotune_engine_status_t.state != IDLE.
    ``zones_heating``: count of ZoneExecStatus entries with
        relay_commanded_on True -- 0 whenever nothing is actually calling for
        heat even if a profile happens to be RUNNING (e.g. between segments).
    ``observations``: sample_count from whichever engine is live
        (autotune's sample_count if autotune_active, else the profile's own
        segment_elapsed_s-derived tick count) -- 0 on a genuinely idle board,
        which is the field this finding's "observations=0" note refers to.
    """

    firing_active: bool
    autotune_active: bool
    zones_heating: int
    observations: int

    @property
    def is_idle(self) -> bool:
        """True only when nothing the deep call chains above depend on was
        happening: no firing, no autotune, no zone actually calling for
        heat. This -- not the freeform ``condition`` string -- is what
        check_stack_margin_baseline.py now grades a checked-in LOW/OK entry
        against."""
        return not self.firing_active and not self.autotune_active and self.zones_heating == 0

    def to_json_dict(self) -> dict:
        return asdict(self)

    @staticmethod
    def from_json_dict(d: dict) -> "LoadSnapshot":
        return LoadSnapshot(
            firing_active=bool(d["firing_active"]),
            autotune_active=bool(d["autotune_active"]),
            zones_heating=int(d["zones_heating"]),
            observations=int(d["observations"]),
        )


@dataclass(frozen=True)
class StackMarginBaselineRecord:
    """One capture: every registered task's HWM under one load condition,
    tagged with the firmware build it was taken against so a later reader
    can tell whether two captures are even comparable."""

    condition: str
    captured_at_utc: str
    fw_commit: str
    fw_dirty: bool
    fw_built: str
    notes: str
    entries: tuple[StackMarginEntry, ...]
    #: Structured load condition (see LoadSnapshot). Optional only for
    #: backward compatibility with baseline files captured before this field
    #: existed -- a NEW capture always fills it in from the board's own
    #: state; see capture_stack_margin_baseline.py.
    load: Optional[LoadSnapshot] = None

    def to_json_dict(self) -> dict:
        d = asdict(self)
        d["entries"] = [
            {**asdict(e), "level": e.level.name} for e in self.entries
        ]
        d["load"] = self.load.to_json_dict() if self.load is not None else None
        return d


def build_record(
    condition: str,
    entries: Iterable[StackMarginEntry],
    fw_version: FirmwareVersion,
    notes: str = "",
    *,
    load: Optional[LoadSnapshot] = None,
    now: Optional[datetime] = None,
) -> StackMarginBaselineRecord:
    """Pure: turns an already-fetched entry list + firmware version into a
    record. Callers doing the real UART round trip pass what
    ``KilnInfo.get_stack_margin()``/``get_fw_version()`` returned; a test
    passes hand-built ``StackMarginEntry``/``FirmwareVersion`` objects --
    neither this function nor anything else in this module ever touches a
    link.

    ``load``, if given, must be a :class:`LoadSnapshot` built by the caller
    from the board's own ProfileExecStatus/AutotuneStatus -- never construct
    one from the ``condition`` string or any other caller intent; that
    defeats the entire point of recording it separately."""
    if condition not in LOAD_CONDITIONS:
        raise ValueError(
            f"unknown load condition {condition!r} -- must be one of {LOAD_CONDITIONS} "
            f"(DRAM_PSRAM_PLAN.md section 4.3). If a new condition is genuinely needed, "
            f"add it to LOAD_CONDITIONS with the same reasoning the other three carry."
        )
    ts = (now or datetime.now(timezone.utc)).strftime("%Y-%m-%dT%H:%M:%SZ")
    return StackMarginBaselineRecord(
        condition=condition,
        captured_at_utc=ts,
        fw_commit=fw_version.commit,
        fw_dirty=fw_version.dirty,
        fw_built=fw_version.built,
        notes=notes,
        entries=tuple(entries),
        load=load,
    )


def record_filename(record: StackMarginBaselineRecord) -> str:
    """``stack_margin_<condition>_<commit>_<timestamp>.json`` -- commit and
    timestamp both in the name so a directory listing alone tells you
    whether two captures came from the same build without opening either
    file."""
    commit = record.fw_commit or "unknown"
    ts = record.captured_at_utc.replace(":", "").replace("-", "")
    return f"stack_margin_{record.condition}_{commit}_{ts}.json"


def write_record(record: StackMarginBaselineRecord, out_dir: Path) -> Path:
    """The only function in this module that touches the filesystem (still
    no network). Returns the path written."""
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / record_filename(record)
    path.write_text(json.dumps(record.to_json_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def load_records(out_dir: Path) -> list[StackMarginBaselineRecord]:
    """Reads every ``stack_margin_*.json`` in ``out_dir`` back into records,
    for :func:`render_markdown_table` to combine. Skips (with no error) any
    file that fails to parse -- a partially-written capture must not take
    down every later report."""
    records: list[StackMarginBaselineRecord] = []
    for path in sorted(out_dir.glob("stack_margin_*.json")):
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            entries = tuple(
                StackMarginEntry(
                    name=e["name"],
                    configured_stack_bytes=e["configured_stack_bytes"],
                    hwm_bytes=e["hwm_bytes"],
                    alive=e["alive"],
                    level=StackMarginLevel[e["level"]],
                )
                for e in raw["entries"]
            )
            raw_load = raw.get("load")
            load = LoadSnapshot.from_json_dict(raw_load) if raw_load else None
            records.append(
                StackMarginBaselineRecord(
                    condition=raw["condition"],
                    captured_at_utc=raw["captured_at_utc"],
                    fw_commit=raw["fw_commit"],
                    fw_dirty=raw["fw_dirty"],
                    fw_built=raw["fw_built"],
                    notes=raw.get("notes", ""),
                    entries=entries,
                    load=load,
                )
            )
        except Exception:  # noqa: BLE001 -- one bad file must not sink the report
            continue
    return records


def worst_case_across_conditions(
    records: Iterable[StackMarginBaselineRecord],
) -> dict[str, StackMarginEntry]:
    """For each task name, the entry with the SMALLEST ``hwm_bytes`` seen
    across every record passed in (i.e. across whichever conditions were
    captured) -- the number DRAM_PSRAM_PLAN.md section 3.1 wants, since a
    task's real worst case is the worst of every load it has actually been
    measured under, not just the last capture taken. Dead (``alive=False``)
    entries are ignored for a task that has at least one live reading;
    a task with only dead readings is reported dead."""
    best: dict[str, StackMarginEntry] = {}
    for rec in records:
        for e in rec.entries:
            cur = best.get(e.name)
            if cur is None:
                best[e.name] = e
                continue
            if not e.alive:
                continue
            if not cur.alive or e.hwm_bytes < cur.hwm_bytes:
                best[e.name] = e
    return best


def render_markdown_table(records: Iterable[StackMarginBaselineRecord]) -> str:
    """Renders the worst-case-across-conditions table in the exact column
    shape DRAM_PSRAM_PLAN.md section 3.1 already uses, so a capture's output
    can be pasted into the plan without reformatting."""
    records = list(records)
    worst = worst_case_across_conditions(records)
    conditions_seen = sorted({r.condition for r in records})
    lines = [
        "| task | free at worst | allocated | headroom | condition(s) captured |",
        "|---|---|---|---|---|",
    ]
    for name in sorted(worst):
        e = worst[name]
        if not e.alive:
            lines.append(f"| `{name}` | -- | {e.configured_stack_bytes} B | not running | -- |")
            continue
        pct = e.headroom_pct
        pct_str = f"{pct:.1f}%" if pct is not None else "n/a"
        lines.append(
            f"| `{name}` | {e.hwm_bytes} B | {e.configured_stack_bytes} B | {pct_str} | "
            f"{', '.join(conditions_seen) if conditions_seen else '--'} |"
        )
    return "\n".join(lines)
