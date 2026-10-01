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
import re
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


#: Matches mcp_server_info.py's get_stack_margin() text rendering exactly:
#: "<name>: <hwm> B free at worst of <configured> B (<pct> headroom) [<LEVEL>]"
_ALIVE_LINE_RE = re.compile(
    r"^(?P<name>\S+):\s+(?P<hwm>\d+)\s+B free at worst of\s+(?P<configured>\d+)\s+B\s+"
    r"\([^)]*\)\s+\[(?P<level>[A-Z]+)\]\s*$"
)
#: "<name>: not running (configured <configured> B)"
_DEAD_LINE_RE = re.compile(r"^(?P<name>\S+):\s+not running\s+\(configured\s+(?P<configured>\d+)\s+B\)\s*$")


def parse_stack_margin_report_text(text: str) -> list[StackMarginEntry]:
    """Reconstructs the ``StackMarginEntry`` list from the get_stack_margin()
    MCP tool's plain-text rendering (mcp_server_info.py) -- for a caller that
    only has that text (e.g. the read-only ``kiln_call(name="get_stack_margin")``
    facade) and never opened its own link to the board's structured
    ``KilnInfo.get_stack_margin()`` reply. Pure text parsing, no I/O, no
    board access -- the counterpart of ``capture_stack_margin_baseline.py``'s
    live capture path for a caller that is deliberately kept off the wire.

    Ignores a trailing ``[STALE MCP SERVER]`` banner line or any other line
    that does not match either known shape, rather than raising, so a caller
    can pass the tool's raw ``result`` string unedited. Raises ``ValueError``
    only if NOTHING in the text parses as an entry -- an empty result is not
    silently accepted as "zero tasks", since ``get_stack_margin()`` itself
    reports that case as ``"device reported no instrumented tasks"``, a
    string this function also refuses to treat as an empty-but-valid list."""
    entries: list[StackMarginEntry] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        m = _ALIVE_LINE_RE.match(line)
        if m:
            entries.append(
                StackMarginEntry(
                    name=m.group("name"),
                    configured_stack_bytes=int(m.group("configured")),
                    hwm_bytes=int(m.group("hwm")),
                    alive=True,
                    level=StackMarginLevel[m.group("level")],
                )
            )
            continue
        m = _DEAD_LINE_RE.match(line)
        if m:
            entries.append(
                StackMarginEntry(
                    name=m.group("name"),
                    configured_stack_bytes=int(m.group("configured")),
                    hwm_bytes=0,
                    alive=False,
                    level=StackMarginLevel.OK,
                )
            )
            continue
        # Anything else (a stale-server banner, a blank separator, an error
        # string) is skipped rather than raising -- see docstring.
    if not entries:
        raise ValueError(
            "no stack-margin entries parsed from the given text -- it does not match "
            "get_stack_margin()'s known line shapes (mcp_server_info.py); pass its raw "
            "'result' string unedited, not an error message or an empty report"
        )
    return entries


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


def records_matching_commit(
    records: Iterable[StackMarginBaselineRecord], fw_commit: Optional[str],
) -> list[StackMarginBaselineRecord]:
    """Committed records whose ``fw_commit`` exactly equals ``fw_commit`` --
    the filter a live comparison (SK-01/SK-02) must apply *before* computing
    a worst case, so an old build's lower reading never gets mixed into a
    live comparison for a different, newer build just because both files
    happen to sit in the same directory (2026-10-01 finding: once a baseline
    existed for the running commit, ``worst_case_across_conditions`` was
    still being handed every committed record regardless of commit, so the
    comparison silently kept using the lowest reading from any build ever
    captured -- see docs/BENCH_TEST_SYSTEM_PLAN.md SK-01/SK-02).

    Returns an empty list (never raises) when ``fw_commit`` is falsy/unknown
    or no record matches -- the caller is expected to treat an empty result
    as "no same-commit baseline", i.e. INCONCLUSIVE, not as "compare against
    nothing and pass by default"."""
    if not fw_commit:
        return []
    return [r for r in records if r.fw_commit == fw_commit]


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


# ---------------------------------------------------------------------------
# Pico (SaftyFW) baseline records -- plan section 7 owner decision 4 ("Pico
# stack threshold"): capture a real baseline in wave 1 while SK-03 keeps its
# flat 25%-free interim rule meanwhile. A separate record type, not a reuse
# of StackMarginBaselineRecord above: the Pico's own
# GET /api/saftyfw_stack_margin report is WORDS, not bytes (see
# safety_stack_margin_http.c's own "opposite convention" comment), is a
# FLOOR since the Pico's last boot rather than a per-capture high-water mark
# tied to one of the three ESP LOAD_CONDITIONS, and is keyed by SaftyFW's
# own build identity (git commit + build date/time -- SaftyFW has no
# HTTP-reported build timestamp the way the ESP's fw_build is), not an ESP
# FirmwareVersion. Folding the two into one shape would either lose that
# units distinction or force a lot of ESP-only fields (condition, load) to
# be meaningless on every Pico record.
#
# Backwards compatibility: this is purely additive. load_records()'s glob
# (stack_margin_*.json) still matches these files' names too, but
# load_records() reads them through StackMarginEntry's ESP-shaped keys
# (configured_stack_bytes, hwm_bytes, alive, level) -- a Pico record's
# entries carry stack_total_words/high_water_words/measured instead, so the
# KeyError that raises is caught by load_records()'s existing "one bad file
# must not sink the report" except-continue, and a Pico record is silently
# skipped by the ESP loader exactly the way a corrupted/partial file
# already is. Use load_pico_records() to read these back correctly, and
# every existing ESP-only record (no "load" field, no "processor" field)
# still loads unchanged through load_records() -- nothing above this
# comment changed shape.


@dataclass(frozen=True)
class PicoStackMarginEntry:
    """One task's reading from GET /api/saftyfw_stack_margin (see
    safety_stack_margin_http.c's safety_stack_margin_build_json() for the
    wire shape this mirrors 1:1). measured False means the poller has not
    yet obtained a reading for this task this boot
    (KILNLINK_STACK_MARGIN_UNMEASURED) -- high_water_words/
    stack_total_words are meaningless (not merely absent) in that case,
    matching the JSON's own shape where those two keys are omitted
    entirely when measured is false."""

    task_id: int
    name: str
    measured: bool
    high_water_words: Optional[int] = None
    stack_total_words: Optional[int] = None

    @property
    def free_fraction(self) -> Optional[float]:
        """high_water_words / stack_total_words -- the flat 25% rule SK-03
        applies today (plan section 7 owner decision 4) compares against
        this, not a byte figure; the units note in
        safety_stack_margin_http.c applies here too. None when unmeasured
        or when stack_total_words is 0 (never divide by an unconfigured
        stack and call the result a real fraction)."""
        if not self.measured or not self.stack_total_words:
            return None
        return self.high_water_words / self.stack_total_words

    def to_json_dict(self) -> dict:
        return asdict(self)

    @staticmethod
    def from_json_dict(d: dict) -> "PicoStackMarginEntry":
        return PicoStackMarginEntry(
            task_id=int(d["task_id"]),
            name=d["name"],
            measured=bool(d["measured"]),
            high_water_words=d.get("high_water_words"),
            stack_total_words=d.get("stack_total_words"),
        )


@dataclass(frozen=True)
class PicoStackMarginBaselineRecord:
    """One capture of every SaftyFW task's stack-margin floor, tagged with
    the SaftyFW build identity it was taken against (git commit + build
    date/time -- the same identity elf_archive.archive_safty_elf() and
    find_safty_crash_elf() key on, so a record and an archived ELF for the
    same boot can always be cross-referenced by identity alone)."""

    captured_at_utc: str
    saftyfw_commit: str
    saftyfw_build_date: str
    saftyfw_build_time: str
    rounds_completed: int
    last_tick_ms: int
    all_measured: bool
    notes: str
    entries: tuple[PicoStackMarginEntry, ...]
    #: A marker field so a reader iterating a directory of mixed ESP/Pico
    #: files can tell the two apart from the parsed dict alone, without
    #: guessing from the filename -- ESP records have no such key at all
    #: (StackMarginBaselineRecord.to_json_dict() sets no "processor" key),
    #: so d.get("processor") == "pico" is a safe test either way.
    processor: str = "pico"

    def to_json_dict(self) -> dict:
        d = asdict(self)
        d["entries"] = [asdict(e) for e in self.entries]
        return d


def build_pico_record(
    entries: Iterable[PicoStackMarginEntry],
    saftyfw_commit: str,
    saftyfw_build_date: str,
    saftyfw_build_time: str,
    rounds_completed: int,
    last_tick_ms: int,
    all_measured: bool,
    notes: str = "",
    *,
    now: Optional[datetime] = None,
) -> PicoStackMarginBaselineRecord:
    """Pure, same contract as build_record(): takes only already-fetched
    data (whatever GET /api/saftyfw_stack_margin returned, decoded), never
    touches the link itself."""
    ts = (now or datetime.now(timezone.utc)).strftime("%Y-%m-%dT%H:%M:%SZ")
    return PicoStackMarginBaselineRecord(
        captured_at_utc=ts,
        saftyfw_commit=saftyfw_commit,
        saftyfw_build_date=saftyfw_build_date,
        saftyfw_build_time=saftyfw_build_time,
        rounds_completed=rounds_completed,
        last_tick_ms=last_tick_ms,
        all_measured=all_measured,
        entries=tuple(entries),
        notes=notes,
    )


def pico_record_filename(record: PicoStackMarginBaselineRecord) -> str:
    """stack_margin_pico_<commit>_<timestamp>.json -- deliberately still
    prefixed stack_margin_ (not a wholly distinct prefix) so a directory
    listing groups ESP and Pico captures together by eye; the "pico" infix
    plus the processor field above are what keeps a reader from conflating
    them, not the filename alone."""
    commit = record.saftyfw_commit or "unknown"
    ts = record.captured_at_utc.replace(":", "").replace("-", "")
    return f"stack_margin_pico_{commit}_{ts}.json"


def write_pico_record(record: PicoStackMarginBaselineRecord, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / pico_record_filename(record)
    path.write_text(json.dumps(record.to_json_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def load_pico_records(out_dir: Path) -> list[PicoStackMarginBaselineRecord]:
    """Reads every stack_margin_pico_*.json back -- a distinct glob from
    load_records()'s (stack_margin_*.json) so this loader never even
    attempts an ESP-shaped file; see this section's docstring for why the
    reverse (load_records() attempting a Pico file) is already safe by
    construction. Same "skip, don't sink the report" contract on a
    corrupted/partial file."""
    records: list[PicoStackMarginBaselineRecord] = []
    for path in sorted(out_dir.glob("stack_margin_pico_*.json")):
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            entries = tuple(PicoStackMarginEntry.from_json_dict(e) for e in raw["entries"])
            records.append(
                PicoStackMarginBaselineRecord(
                    captured_at_utc=raw["captured_at_utc"],
                    saftyfw_commit=raw["saftyfw_commit"],
                    saftyfw_build_date=raw["saftyfw_build_date"],
                    saftyfw_build_time=raw["saftyfw_build_time"],
                    rounds_completed=int(raw["rounds_completed"]),
                    last_tick_ms=int(raw["last_tick_ms"]),
                    all_measured=bool(raw["all_measured"]),
                    notes=raw.get("notes", ""),
                    entries=entries,
                    processor=raw.get("processor", "pico"),
                )
            )
        except Exception:  # noqa: BLE001 -- one bad file must not sink the report
            continue
    return records
