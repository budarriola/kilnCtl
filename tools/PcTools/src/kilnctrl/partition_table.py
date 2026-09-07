"""Read, parse, and diff the ESP32 partition table -- FLASH_BUDGET.md
section 8 item 3 ("confirm what table is actually on the chip").

STATUS (2026-09-02): the original chip-read mechanism here -- raw
partition-table bytes over JTAG at flash offset 0x8000, via
``debug_probe.read_memory`` -- DOES NOT WORK. Confirmed against the real
board:

    failed to read 4096 B from esp flash at 0x8000
    DEPRECATED! use 'read_memory' not 'mem2array'
    failed to read memory

0x8000 is a FLASH offset, not a memory-mapped address on the ESP32-S3 --
OpenOCD's ``read_memory`` reaches the CPU's address space, which cannot
reach raw flash content at an arbitrary offset. This module's own tests
never caught it because they inject a fake ``read_memory_fn`` that stands
in for the OpenOCD call and never exercises a real board.
``read_chip_partition_table_bytes()``/``check_chip_partition_table()``
below are KEPT (their parse logic is correct and remains unit-tested
against synthetic blobs) but are DEPRECATED as a chip-read mechanism -- see
each function's own docstring. Nobody should call them expecting a real
chip read to succeed.

The current mechanism is ``check_chip_partition_table_via_http()`` /
``read_chip_partition_table_from_http()`` below, which ask the RUNNING
FIRMWARE for its own live partition table over GET /api/partitions
(``firmware/KilnFW/App/drivers/http/partition_info_http.c``, via ESP-IDF's
``esp_partition_find``/``esp_partition_next`` from inside the app -- no
JTAG, no core halt, works while the board is busy). It also answers a
better question: not "what raw bytes sit at 0x8000" but "what table is the
firmware actually using right now" (including which OTA slot is running).

Nothing else readable over the board's HTTP surface reported the on-chip
partition table before ``/api/partitions`` was added, and
``check_flash_partition_map.ps1`` only validates the REPO's
``partitions.csv`` -- it never touches hardware. Both the JTAG path (kept,
deprecated) and the HTTP path (current) share the same parse/diff core
below: the standard ESP-IDF binary partition-entry format for the JTAG
path, the JSON shape documented on ``read_chip_partition_table_from_http``
for the HTTP path, and always the same ``partitions.csv`` parser and
``diff_partition_tables()``/``PartitionDiff`` for the comparison against
the repo's CSV.

Binary format (``components/partition_table/gen_esp32part.py`` in ESP-IDF),
32 bytes per entry, little-endian:

    offset  size  field
    0       2     magic (0xAA50 for a partition entry, 0xEBEB for the
                   optional MD5-checksum entry, 0xFFFF for unprogrammed/
                   end-of-table)
    2       1     type
    3       1     subtype
    4       4     offset
    8       4     size
    12      16    label (NUL-padded ASCII)
    28      4     flags

This module is pure parsing/diffing logic with no hardware dependency of its
own -- ``read_chip_partition_table_bytes()`` is the one function that touches
``debug_probe``, everything else is plain bytes/CSV in, data out, so the
diff logic is unit-testable against synthetic blobs (see
``tests/test_partition_table.py``) without a board attached.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from typing import Optional

# --- Binary format constants -------------------------------------------------

ENTRY_SIZE = 32
ENTRY_MAGIC = 0xAA50
MD5_MAGIC = 0xEBEB
END_MAGIC = 0xFFFF  # unprogrammed flash -- what a fresh/erased region reads as

#: Default location of the partition table itself on this board (see
#: partitions.csv's own header comment: "0x008000..0x009000 partition table
#: itself (CONFIG_PARTITION_TABLE_OFFSET)"). ESP-IDF reserves one 4K sector
#: for it regardless of how many entries are actually used.
DEFAULT_TABLE_ADDRESS = 0x8000
DEFAULT_TABLE_SIZE = 0x1000  # one flash sector, matches the CSV comment above

# gen_esp32part.py's type/subtype string -> numeric encoding. Kept local
# (not imported from ESP-IDF) since this module has no ESP-IDF Python
# dependency and these values are part of the stable on-flash ABI, not
# something that varies by IDF version.
_TYPE_NUM = {"app": 0x00, "data": 0x01}
_APP_SUBTYPE_NUM = {"factory": 0x00, "test": 0x20}
_DATA_SUBTYPE_NUM = {
    "ota": 0x00,
    "phy": 0x01,
    "nvs": 0x02,
    "coredump": 0x03,
    "nvs_keys": 0x04,
    "efuse": 0x05,
    "undefined": 0x06,
    "esphttpd": 0x80,
    "fat": 0x81,
    "spiffs": 0x82,
    # 0x83 per ESP-IDF v6 components/partition_table/gen_esp32part.py. Added
    # 2026-09-07 with the `cfg` LittleFS partition -- without it every tool
    # that parses partitions.csv (debug_check_partition_table included) fails
    # the whole table with "unknown partition subtype 'littlefs'".
    "littlefs": 0x83,
}


def _resolve_type(text: str) -> int:
    t = text.strip().lower()
    if t in _TYPE_NUM:
        return _TYPE_NUM[t]
    if t.startswith("0x"):
        return int(t, 16)
    if t.isdigit():
        return int(t)
    raise ValueError(f"unknown partition type {text!r}")


def _resolve_subtype(type_num: int, text: str) -> int:
    t = text.strip().lower()
    if type_num == _TYPE_NUM["app"]:
        if t in _APP_SUBTYPE_NUM:
            return _APP_SUBTYPE_NUM[t]
        # ota_0 .. ota_15 -> 0x10 + n (gen_esp32part.py's OTA-slot encoding)
        if t.startswith("ota_") and t[4:].isdigit():
            n = int(t[4:])
            if 0 <= n <= 15:
                return 0x10 + n
    else:
        if t in _DATA_SUBTYPE_NUM:
            return _DATA_SUBTYPE_NUM[t]
    if t.startswith("0x"):
        return int(t, 16)
    if t.isdigit():
        return int(t)
    raise ValueError(f"unknown partition subtype {text!r} for type 0x{type_num:02x}")


@dataclass(frozen=True)
class PartitionEntry:
    name: str
    type: int
    subtype: int
    offset: int
    size: int
    flags: int = 0

    @property
    def end(self) -> int:
        return self.offset + self.size

    def field_str(self) -> str:
        return (
            f"{self.name} type=0x{self.type:02x} subtype=0x{self.subtype:02x} "
            f"offset=0x{self.offset:x} size=0x{self.size:x} ({self.size} B)"
        )


# --- Binary parsing -----------------------------------------------------------


def parse_partition_table_binary(data: bytes, base_address: int = DEFAULT_TABLE_ADDRESS) -> "list[PartitionEntry]":
    """Parses raw partition-table bytes (as read from flash at
    ``base_address``) into a list of :class:`PartitionEntry`.

    Stops at the first entry whose magic is not ``0xAA50`` -- that is either
    the optional MD5-checksum entry (``0xEBEB``) or unprogrammed flash
    (``0xFFFF``), and in either case there are no more real partition
    entries after it. Raises ``ValueError`` on a magic that is none of the
    three recognized values (a genuinely corrupt/garbage table), and on a
    truncated final entry (fewer than 32 bytes remaining).
    """
    entries: "list[PartitionEntry]" = []
    offset = 0
    while offset + ENTRY_SIZE <= len(data):
        chunk = data[offset : offset + ENTRY_SIZE]
        magic = struct.unpack_from("<H", chunk, 0)[0]
        if magic in (MD5_MAGIC, END_MAGIC):
            break
        if magic != ENTRY_MAGIC:
            raise ValueError(
                f"bad partition-table magic 0x{magic:04x} at table offset 0x{offset:x} "
                f"(flash address 0x{base_address + offset:x}) -- expected 0x{ENTRY_MAGIC:04x}, "
                f"0x{MD5_MAGIC:04x} (checksum), or 0x{END_MAGIC:04x} (unprogrammed)"
            )
        ptype, subtype = struct.unpack_from("<BB", chunk, 2)
        part_offset, size = struct.unpack_from("<II", chunk, 4)
        label_raw = chunk[12:28]
        label = label_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        flags = struct.unpack_from("<I", chunk, 28)[0]
        entries.append(PartitionEntry(label, ptype, subtype, part_offset, size, flags))
        offset += ENTRY_SIZE
    if not entries and len(data) >= ENTRY_SIZE:
        # Not an error by itself (a genuinely empty/erased table would look
        # like this too), but almost certainly means the read address was
        # wrong -- surface it as such rather than a silent empty list.
        magic = struct.unpack_from("<H", data, 0)[0]
        if magic == END_MAGIC:
            raise ValueError(
                f"no partition entries found at 0x{base_address:x} -- first bytes read as "
                f"unprogrammed flash (0x{END_MAGIC:04x}). Wrong address, or the table was erased?"
            )
    return entries


# --- CSV parsing (mirrors check_flash_partition_map.ps1's ConvertFrom-PartitionSize) ---


def _parse_size_field(text: str, field_name: str, row_context: str) -> int:
    t = text.strip()
    if not t:
        raise ValueError(f"empty {field_name} in row: {row_context}")
    if t.lower().startswith("0x"):
        return int(t, 16)
    if len(t) >= 2 and t[-1].lower() in ("k", "m") and t[:-1].isdigit():
        num = int(t[:-1])
        return num * (1024 if t[-1].lower() == "k" else 1024 * 1024)
    if t.isdigit():
        return int(t)
    raise ValueError(f"cannot parse {field_name} {text!r} in row: {row_context}")


def parse_partitions_csv(path: str) -> "list[PartitionEntry]":
    """Parses an ESP-IDF ``partitions.csv`` into :class:`PartitionEntry`
    objects, resolving the CSV's symbolic type/subtype strings (``app``,
    ``data``, ``nvs``, ``ota_0``, ...) to the same numeric encoding the
    on-flash binary table uses, so the two can be diffed field-by-field.

    Comment lines (``#...``) and blank lines are skipped, same as
    ``check_flash_partition_map.ps1``. Requires at least 5 comma-separated
    fields (Name, Type, SubType, Offset, Size); a 6th (Flags) is accepted
    but currently ignored in the diff (the CSV in this repo never sets one).
    """
    entries: "list[PartitionEntry]" = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            trimmed = line.strip()
            if not trimmed or trimmed.startswith("#"):
                continue
            fields = [x.strip() for x in trimmed.split(",")]
            if len(fields) < 5:
                raise ValueError(f"malformed row (need at least 5 fields) in {path}: {line!r}")
            name, type_s, subtype_s, offset_s, size_s = fields[:5]
            type_num = _resolve_type(type_s)
            subtype_num = _resolve_subtype(type_num, subtype_s)
            offset = _parse_size_field(offset_s, "Offset", line)
            size = _parse_size_field(size_s, "Size", line)
            entries.append(PartitionEntry(name, type_num, subtype_num, offset, size))
    if not entries:
        raise ValueError(f"no partition rows found in {path}")
    return entries


# --- Diff ----------------------------------------------------------------


@dataclass(frozen=True)
class PartitionDiff:
    only_on_chip: "list[PartitionEntry]"
    only_in_csv: "list[PartitionEntry]"
    # (name, [field mismatch strings])
    mismatched: "list[tuple[str, list[str]]]"

    @property
    def ok(self) -> bool:
        return not self.only_on_chip and not self.only_in_csv and not self.mismatched

    def report(self) -> str:
        lines: "list[str]" = []
        if self.ok:
            lines.append("MATCH: on-chip partition table matches partitions.csv exactly.")
            return "\n".join(lines)
        lines.append("MISMATCH: on-chip partition table does NOT match partitions.csv.")
        for entry in self.only_on_chip:
            lines.append(f"  ONLY ON CHIP (not in partitions.csv): {entry.field_str()}")
        for entry in self.only_in_csv:
            lines.append(f"  ONLY IN partitions.csv (not on chip): {entry.field_str()}")
        for name, field_mismatches in self.mismatched:
            lines.append(f"  '{name}':")
            for m in field_mismatches:
                lines.append(f"    {m}")
        return "\n".join(lines)


def diff_partition_tables(chip_entries: "list[PartitionEntry]", csv_entries: "list[PartitionEntry]") -> PartitionDiff:
    """Compares two partition-entry lists (matched by ``name``) and reports,
    entry by entry:

      - partitions present on the chip but not in the CSV (or vice versa)
      - for partitions present in both, any of type/subtype/offset/size that
        differ

    Order-independent (matches by name, not position) -- the on-chip table's
    entry order is whatever ``gen_esp32part.py`` wrote it in, which need not
    equal the CSV's row order for the comparison to be meaningful."""
    chip_by_name = {e.name: e for e in chip_entries}
    csv_by_name = {e.name: e for e in csv_entries}

    only_on_chip = [chip_by_name[n] for n in chip_by_name if n not in csv_by_name]
    only_in_csv = [csv_by_name[n] for n in csv_by_name if n not in chip_by_name]

    mismatched: "list[tuple[str, list[str]]]" = []
    for name in sorted(set(chip_by_name) & set(csv_by_name)):
        chip_e = chip_by_name[name]
        csv_e = csv_by_name[name]
        field_mismatches: "list[str]" = []
        if chip_e.type != csv_e.type:
            field_mismatches.append(f"type: chip=0x{chip_e.type:02x} csv=0x{csv_e.type:02x}")
        if chip_e.subtype != csv_e.subtype:
            field_mismatches.append(f"subtype: chip=0x{chip_e.subtype:02x} csv=0x{csv_e.subtype:02x}")
        if chip_e.offset != csv_e.offset:
            field_mismatches.append(f"offset: chip=0x{chip_e.offset:x} csv=0x{csv_e.offset:x}")
        if chip_e.size != csv_e.size:
            field_mismatches.append(f"size: chip={chip_e.size} B (0x{chip_e.size:x}) csv={csv_e.size} B (0x{csv_e.size:x})")
        if field_mismatches:
            mismatched.append((name, field_mismatches))

    return PartitionDiff(only_on_chip=only_on_chip, only_in_csv=only_in_csv, mismatched=mismatched)


# --- Hardware read (the only function here that touches debug_probe/OpenOCD) --

_MEMRD_LINE_PREFIX = "MEMRD "


def _bytes_from_memrd_output(output: str, expected_count: int) -> bytes:
    """Parses ``debug_probe.read_memory(..., width=8)``'s ``MEMRD <addr>
    <byte>`` text output (see that function's own docstring for why it uses
    ``puts``/``MEMRD`` rather than OpenOCD's interactive ``mdb``) into raw
    bytes, ordered by address."""
    by_addr: "dict[int, int]" = {}
    for line in output.splitlines():
        line = line.strip()
        if not line.startswith(_MEMRD_LINE_PREFIX):
            continue
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            addr = int(parts[1], 16)
            val = int(parts[2], 16)
        except ValueError:
            continue
        by_addr[addr] = val
    if len(by_addr) != expected_count:
        raise ValueError(
            f"expected {expected_count} MEMRD byte(s) in openocd output, parsed {len(by_addr)} -- "
            f"raw output tail:\n" + "\n".join(output.strip().splitlines()[-10:])
        )
    ordered_addrs = sorted(by_addr)
    return bytes(by_addr[a] for a in ordered_addrs)


def read_chip_partition_table_bytes(
    peer: str = "esp",
    address: int = DEFAULT_TABLE_ADDRESS,
    size: int = DEFAULT_TABLE_SIZE,
    read_memory_fn=None,
) -> bytes:
    """DEPRECATED as a real chip-read mechanism -- see this module's
    docstring. ``debug_probe.read_memory`` reaches the CPU's memory-mapped
    address space, not raw flash content at an arbitrary offset; against
    the real board this fails with "failed to read 4096 B from esp flash at
    0x8000" / "failed to read memory". Kept only because its parsing logic
    (``_bytes_from_memrd_output``) is correct and still unit-tested; do not
    wire this into anything expecting a real read to succeed. Use
    ``read_chip_partition_table_from_http()`` instead.

    Reads ``size`` raw bytes starting at ``address`` from ``peer``'s flash
    over JTAG/SWD (via ``debug_probe.read_memory``, width=8) and returns them
    as a ``bytes`` object.

    ``read_memory_fn`` defaults to ``debug_probe.read_memory`` -- overridable
    so callers (and this module's own tests) can inject a fake without a
    board attached. Imports ``debug_probe`` lazily so this module stays
    importable (and its pure parse/diff functions testable) in an
    environment with no OpenOCD/board dependency available at all.

    This halts the target core briefly to perform the read (OpenOCD requires
    a halted core for a memory read) and resumes it afterwards -- see
    ``debug_probe.read_memory``'s own docstring. It is a flash READ, not a
    write: nothing on the chip is modified.
    """
    if read_memory_fn is None:
        from . import debug_probe

        read_memory_fn = debug_probe.read_memory
    ok, output = read_memory_fn(peer, address, count=size, width=8)
    if not ok:
        raise RuntimeError(f"failed to read {size} B from {peer} flash at 0x{address:x}:\n{output.strip()}")
    return _bytes_from_memrd_output(output, size)


# --- Top-level convenience: read + parse both sides + diff --------------------


def check_chip_partition_table(
    peer: str = "esp",
    csv_path: Optional[str] = None,
    address: int = DEFAULT_TABLE_ADDRESS,
    size: int = DEFAULT_TABLE_SIZE,
    read_memory_fn=None,
) -> "tuple[PartitionDiff, list[PartitionEntry], list[PartitionEntry]]":
    """DEPRECATED as a real chip-read mechanism -- see this module's
    docstring and ``read_chip_partition_table_bytes()``'s own deprecation
    notice. Use ``check_chip_partition_table_via_http()`` instead; that is
    what the MCP tool (``debug_check_partition_table``) and the CLI script
    (``check_chip_partition_table.py``) call now.

    Reads the on-chip partition table, parses ``csv_path`` (defaults to
    ``firmware/KilnFW/partitions.csv`` in this repo), and returns
    ``(diff, chip_entries, csv_entries)``.
    """
    if csv_path is None:
        csv_path = _default_csv_path()

    chip_bytes = read_chip_partition_table_bytes(peer, address, size, read_memory_fn=read_memory_fn)
    chip_entries = parse_partition_table_binary(chip_bytes, base_address=address)
    csv_entries = parse_partitions_csv(csv_path)
    diff = diff_partition_tables(chip_entries, csv_entries)
    return diff, chip_entries, csv_entries


# --- HTTP read (current mechanism) -- GET /api/partitions from the running app ---


def _default_csv_path() -> str:
    # tools/PcTools/src/kilnctrl/partition_table.py -> repo root is 4 up
    repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
    return os.path.join(repo_root, "firmware", "KilnFW", "partitions.csv")


def read_chip_partition_table_from_http(
    host: str,
    timeout: float = 5.0,
    get_partitions_fn=None,
) -> "list[PartitionEntry]":
    """Reads the running firmware's own live partition table over
    GET /api/partitions and returns it as :class:`PartitionEntry` objects,
    same shape ``read_chip_partition_table_bytes()`` + ``parse_partition_
    table_binary()`` used to produce from a raw JTAG read -- so
    ``diff_partition_tables()`` and everything downstream of it (including
    ``PartitionDiff.report()``) needs no changes to work with either source.

    ``get_partitions_fn`` defaults to ``partition_http_client.get_partitions``
    -- overridable so callers (and this module's own tests) can inject a
    fake without a real socket or board, same "injected function" shape
    ``read_chip_partition_table_bytes()``'s ``read_memory_fn`` parameter
    used for the JTAG path. Imports ``partition_http_client`` lazily so this
    module stays importable in an environment with no network dependency at
    all.

    Response shape expected from the endpoint (partition_info_http.c):
    ``{"running": "<label>", "partitions": [{"label", "type", "subtype",
    "offset", "size", "encrypted"}, ...]}``. Raises whatever
    ``partition_http_client.PartitionHttpError`` raises on any transport
    failure, non-2xx response, or malformed body -- this function adds no
    further leniency, since a firmware/tool JSON-shape mismatch here should
    fail loudly rather than silently report an empty or partial table.
    """
    if get_partitions_fn is None:
        from . import partition_http_client

        get_partitions_fn = partition_http_client.get_partitions

    data = get_partitions_fn(host, timeout=timeout)
    entries: "list[PartitionEntry]" = []
    for item in data["partitions"]:
        entries.append(
            PartitionEntry(
                name=item["label"],
                type=int(item["type"]),
                subtype=int(item["subtype"]),
                offset=int(item["offset"]),
                size=int(item["size"]),
            )
        )
    return entries


def check_chip_partition_table_via_http(
    host: str,
    csv_path: Optional[str] = None,
    timeout: float = 5.0,
    get_partitions_fn=None,
) -> "tuple[PartitionDiff, list[PartitionEntry], list[PartitionEntry]]":
    """Current top-level convenience: reads the running firmware's live
    partition table over GET /api/partitions, parses ``csv_path`` (defaults
    to ``firmware/KilnFW/partitions.csv``), and returns
    ``(diff, chip_entries, csv_entries)`` -- the HTTP-sourced replacement
    for the now-deprecated ``check_chip_partition_table()``. This is the
    function the MCP tool wrapper (``debug_check_partition_table``) and the
    CLI script (``check_chip_partition_table.py``) call.
    """
    if csv_path is None:
        csv_path = _default_csv_path()

    chip_entries = read_chip_partition_table_from_http(host, timeout=timeout, get_partitions_fn=get_partitions_fn)
    csv_entries = parse_partitions_csv(csv_path)
    diff = diff_partition_tables(chip_entries, csv_entries)
    return diff, chip_entries, csv_entries
