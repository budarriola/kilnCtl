"""Decoder for KilnFW's on-flash binary event log (firmware/KilnFW/App/
drivers/event_log.h).

Since 2026-09-02 (owner decision, FLASH_BUDGET_PLAN.md section 5.2 follow-on)
the board's flash log store (log_store.c) holds fixed 32-byte BINARY event
records -- errors/warnings/info for genuinely-necessary-for-debug events
(run start/pause/resume/done/fault, autotune start/done/aborted), never a
per-tick temperature sample. Per-tick temperature/telemetry debugging lives
on the debug UART instead (telemetry_log.c's ESP_LOGI feed, opt-in via
telemetry_log_set_enabled()), not in this file's scope at all.

This module is the PC-side counterpart to event_log.c's encode/decode: the
board never emits human-readable text for these records, so a human (or a
script) reading them goes through here. It is a straight byte-for-byte port
of event_log_decode() -- same offsets, same magic/version bytes, same
little-endian multi-byte fields -- kept in sync by hand (there is no shared
codegen between the C and Python sides in this project; see crc16.py's own
docstring for the same "documented pure-Python port" pattern).

MIGRATION / OLD-FORMAT LOGS: a flash log store written before 2026-09-02
holds a completely different format -- one NUL-free ASCII/UTF-8 text line
per record ("KTEL1 FIRE ..." / "KTEL1 TUNE ..."), with no magic byte and no
fixed record size at all. This decoder does not attempt to read that
format: every record it decodes must start with EVENT_MAGIC/EVENT_VERSION,
so old-format bytes are refused with EventLogFormatError rather than
misread as (nonsensical) binary fields. If a firing's flash log predates
this change, fetch the OLD /api/logs/firing response (it will read back as
plain text over the wire) before this firmware version's next flash
mount/rotation cycles it out -- there is no automatic converter.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

__all__ = [
    "EVENT_MAGIC",
    "EVENT_VERSION",
    "RECORD_SIZE",
    "NOTE_LEN",
    "ZONE_NONE",
    "Severity",
    "Source",
    "FiringCode",
    "AutotuneCode",
    "EventLogFormatError",
    "EventRecord",
    "decode_record",
    "decode_stream",
    "format_record",
]

# Byte layout (event_log.h / event_log.c, OFF_* constants) -- 32 bytes total:
#   0:      magic    uint8
#   1:      version  uint8
#   2:      severity uint8
#   3:      source   uint8
#   4-7:    uptime_s uint32 LE
#   8:      code     uint8
#   9:      zone     uint8  (0xFF = not zone-specific)
#   10-11:  reserved (always 0, ignored on decode)
#   12-15:  arg      int32 LE
#   16-31:  note     16 bytes, NUL-padded ASCII
_STRUCT = struct.Struct("<BBBBIBBxxi16s")  # 'xx' = the 2 reserved bytes

EVENT_MAGIC = 0xE7
EVENT_VERSION = 1
RECORD_SIZE = _STRUCT.size
NOTE_LEN = 16
ZONE_NONE = 0xFF

assert RECORD_SIZE == 32, f"event_log_decoder/_STRUCT drifted from event_log.h's 32-byte record ({RECORD_SIZE})"


class Severity(IntEnum):
    INFO = 0
    WARN = 1
    ERROR = 2


class Source(IntEnum):
    FIRING = 0
    AUTOTUNE = 1
    SYSTEM = 2


class FiringCode(IntEnum):
    STARTED = 0
    PAUSED = 1
    RESUMED = 2
    DONE = 3
    FAULTED = 4


class AutotuneCode(IntEnum):
    STARTED = 16
    DONE = 17
    ABORTED = 18


class EventLogFormatError(ValueError):
    """Raised when bytes do not decode as a valid event_log.h record --
    wrong length, wrong magic/version (including every pre-2026-09-02
    text-format log), or a truncated trailing record."""


@dataclass(frozen=True)
class EventRecord:
    severity: int
    source: int
    code: int
    zone: int  # ZONE_NONE if not zone-specific
    uptime_s: int
    arg: int
    note: str


def decode_record(data: bytes) -> EventRecord:
    """Decodes exactly RECORD_SIZE bytes into an EventRecord. Raises
    EventLogFormatError if `data` is the wrong length or does not carry the
    current magic/version byte pair -- this NEVER guesses at a record it
    cannot positively identify (see this module's own docstring on why: an
    old-format text log looks like nothing this decoder recognizes, by
    construction, and must be refused loudly rather than misread)."""
    if len(data) != RECORD_SIZE:
        raise EventLogFormatError(f"expected exactly {RECORD_SIZE} bytes, got {len(data)}")

    magic, version, severity, source, uptime_s, code, zone, arg, note_raw = _STRUCT.unpack(data)

    if magic != EVENT_MAGIC or version != EVENT_VERSION:
        raise EventLogFormatError(
            f"unrecognized record format (magic=0x{magic:02X}, version={version}) -- "
            "not this build's event_log format; this is expected for any log written "
            "before the 2026-09-02 binary-event-log change (old text-line format), or "
            "for corrupted/misaligned bytes. Refusing to decode rather than guess."
        )

    note = note_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")
    return EventRecord(
        severity=severity, source=source, code=code, zone=zone, uptime_s=uptime_s, arg=arg, note=note
    )


def decode_stream(data: bytes) -> list[EventRecord]:
    """Decodes a back-to-back stream of records (exactly what GET
    /api/logs/firing or /api/logs/autotune returns -- log_http.c streams
    RECORD_SIZE-byte chunks with no delimiter). Raises EventLogFormatError
    if the stream length is not a whole multiple of RECORD_SIZE (a
    truncated final record) or if any individual record fails to decode."""
    if len(data) % RECORD_SIZE != 0:
        raise EventLogFormatError(
            f"stream length {len(data)} is not a multiple of RECORD_SIZE ({RECORD_SIZE}) -- "
            "truncated final record"
        )
    return [decode_record(data[i : i + RECORD_SIZE]) for i in range(0, len(data), RECORD_SIZE)]


_SEVERITY_NAMES = {Severity.INFO: "INFO", Severity.WARN: "WARN", Severity.ERROR: "ERROR"}
_SOURCE_NAMES = {Source.FIRING: "FIRING", Source.AUTOTUNE: "AUTOTUNE", Source.SYSTEM: "SYSTEM"}
_FIRING_CODE_NAMES = {c.value: c.name for c in FiringCode}
_AUTOTUNE_CODE_NAMES = {c.value: c.name for c in AutotuneCode}


def format_record(rec: EventRecord) -> str:
    """Human-readable one-line rendering of a decoded record -- the text
    form this decoder produces for a human to read, since the board itself
    never emits one any more (event_log.h's file banner)."""
    sev = _SEVERITY_NAMES.get(rec.severity, f"SEV{rec.severity}")
    src = _SOURCE_NAMES.get(rec.source, f"SRC{rec.source}")
    if rec.source == Source.FIRING:
        code = _FIRING_CODE_NAMES.get(rec.code, f"CODE{rec.code}")
    elif rec.source == Source.AUTOTUNE:
        code = _AUTOTUNE_CODE_NAMES.get(rec.code, f"CODE{rec.code}")
    else:
        code = f"CODE{rec.code}"
    zone = "-" if rec.zone == ZONE_NONE else str(rec.zone)
    note = f" note={rec.note!r}" if rec.note else ""
    return f"[{rec.uptime_s:>8}s] {sev:<5} {src:<8} {code:<12} zone={zone} arg={rec.arg}{note}"
