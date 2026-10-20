"""PC side of FD-01: fetch, unpack and decode GET /api/diag/bundle.

Wire format (firmware: App/drivers/http/diag_bundle.h) is a stream of frames

    b"KDF1 " name b" " decimal_len b"\\n" payload[len] b"\\n"

starting with a "_manifest" frame and ending with a "_trailer" frame. A
section may span several frames (concatenated). A bundle with no trailer was
truncated in transit and is reported as such, never silently accepted.

Sections the firmware cannot generate itself (its manifest's
"pc_supplement" list: /api/status, /api/readiness, ...) are fetched here with
the same admin login and written into the output directory as
supplement_<name>.json.
"""

from __future__ import annotations

import http.client
import json
import os
import re
import urllib.error
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import event_log_decoder, http_auth

__all__ = ["BundleError", "Bundle", "parse_bundle", "unpack_bundle", "fetch_bundle", "fetch_and_unpack"]

_NAME_RE = re.compile(r"^[a-z0-9_.\-]{1,31}$")
_FILE_RE = re.compile(r"^[A-Za-z0-9_.\-]+$")
_EVENT_LOGS = ("firing_log", "autotune_log")


class BundleError(RuntimeError):
    pass


@dataclass
class Bundle:
    manifest: dict
    trailer: Optional[dict]
    sections: dict[str, bytes] = field(default_factory=dict)
    truncated: str = ""  # non-empty: the transfer was cut inside a frame; complete frames were kept

    @property
    def complete(self) -> bool:
        return self.trailer is not None and bool(self.trailer.get("complete"))


_LEN_RE = re.compile(rb"^[0-9]{1,10}$")
# Section names the unpacker writes itself or that mark framing frames (DB-07). Mirrors diag_bundle.c.
_RESERVED_NAMES = ("manifest", "trailer", "coredump.symbolize")
MAX_BUNDLE_BYTES = 256 * 1024 * 1024  # DB-08: cap on what fetch_bundle reads
MAX_FRAME_BYTES = 1024 * 1024  # DB-04: mirrors DIAG_FRAME_MAX


def _name_reserved(name: str) -> bool:
    return name.startswith(("_", ".", "supplement_")) or name in _RESERVED_NAMES


def _json(payload: bytes, what: str):
    try:
        return json.loads(payload)
    except (ValueError, UnicodeDecodeError) as exc:
        raise BundleError(f"{what} frame is not valid JSON: {exc}") from exc


def parse_bundle(data: bytes) -> Bundle:
    """Parses a KDIAG1 stream. Structural errors raise BundleError. A cut in the middle of a frame
    (transfer truncated) keeps every complete frame and marks the bundle truncated (DB-08)."""
    pos = 0
    sections: dict[str, bytes] = {}
    manifest = None
    trailer = None
    truncated = ""
    while pos < len(data):
        if trailer is not None:
            raise BundleError(f"data after the _trailer frame at byte {pos}")
        nl = data.find(b"\n", pos)
        if nl < 0:
            truncated = f"truncated frame header at byte {pos}"
            break
        head = data[pos:nl].split(b" ")
        if len(head) != 3 or head[0] != b"KDF1":
            raise BundleError(f"bad frame header at byte {pos}: {data[pos:nl][:40]!r}")
        name = head[1].decode("ascii", "replace")
        if not _NAME_RE.match(name):
            raise BundleError(f"bad section name {name!r}")
        if not _LEN_RE.match(head[2]):
            raise BundleError(f"bad frame length {head[2]!r}")
        length = int(head[2])
        if length > MAX_FRAME_BYTES:
            raise BundleError(f"frame {name!r} length {length} exceeds {MAX_FRAME_BYTES}")
        start = nl + 1
        end = start + length
        if end + 1 > len(data):
            truncated = f"truncated frame {name!r} at byte {pos}"
            break
        if data[end:end + 1] != b"\n":
            raise BundleError(f"unterminated frame {name!r} at byte {pos}")
        payload = data[start:end]
        if pos == 0 and name != "_manifest":
            raise BundleError(f"first frame is {name!r}, not _manifest")
        new_pos = end + 1
        if new_pos <= pos:  # unreachable with the length grammar above; kept as a hard guarantee
            raise BundleError(f"parser made no progress at byte {pos}")
        pos = new_pos
        if name == "_manifest":
            if manifest is not None:
                raise BundleError("duplicate _manifest frame")
            manifest = _json(payload, "_manifest")
        elif name == "_trailer":
            trailer = _json(payload, "_trailer")
        elif _name_reserved(name):
            raise BundleError(f"reserved section name {name!r}")
        else:
            sections[name] = sections.get(name, b"") + payload
    if manifest is None:
        raise BundleError("bundle has no _manifest frame")
    if trailer is not None:
        results = trailer.get("results", []) if isinstance(trailer, dict) else []
        for r in results:
            if not isinstance(r, dict):
                raise BundleError("trailer result is not an object")
            got = len(sections.get(r.get("name"), b""))
            if r.get("bytes") != got:
                raise BundleError(f"trailer says section {r.get('name')!r} has {r.get('bytes')} bytes, received {got}")
    return Bundle(manifest=manifest, trailer=trailer, sections=sections, truncated=truncated)


def _decode_event_log(raw: bytes) -> str:
    usable = len(raw) - (len(raw) % event_log_decoder.RECORD_SIZE)
    lines = [event_log_decoder.format_record(r) for r in event_log_decoder.decode_stream(raw[:usable])]
    if usable != len(raw):
        lines.append(f"# {len(raw) - usable} trailing bytes ignored (partial record)")
    return "\n".join(lines) + ("\n" if lines else "")


def unpack_bundle(bundle: Bundle, out_dir: str, *, elf_paths: Optional[list[str]] = None,
                  symbolize: Optional[Callable[[str, list[str]], str]] = None) -> list[str]:
    """Writes every section into out_dir; returns the list of files written.
    Event logs are decoded to <name>.txt beside the raw .bin. The coredump is
    symbolized with `symbolize(coredump_path, elf_paths)` when both are given;
    a failure is written to coredump.symbolize.txt, never raised."""
    os.makedirs(out_dir, exist_ok=True)
    written: list[str] = []

    root = os.path.realpath(out_dir)

    def put(name: str, content: bytes) -> str:
        if not _FILE_RE.match(name) or ".." in name:
            raise BundleError(f"unsafe output file name {name!r}")
        path = os.path.join(out_dir, name)
        if os.path.dirname(os.path.realpath(path)) != root:
            raise BundleError(f"output path escapes the output directory: {name!r}")
        with open(path, "wb") as f:
            f.write(content)
        written.append(path)
        return path

    put("manifest.json", json.dumps(bundle.manifest, indent=2).encode())
    if bundle.trailer is not None:
        put("trailer.json", json.dumps(bundle.trailer, indent=2).encode())
    for name, raw in bundle.sections.items():
        if name == "coredump":
            core = put("coredump.bin", raw)
            if symbolize is not None and elf_paths:
                try:
                    text = symbolize(core, elf_paths)
                except Exception as exc:  # noqa: BLE001 - reported in the file, not raised
                    text = f"symbolize failed: {exc}\n"
                put("coredump.symbolize.txt", text.encode())
        elif name in _EVENT_LOGS:
            put(name + ".bin", raw)
            try:
                put(name + ".txt", _decode_event_log(raw).encode())
            except event_log_decoder.EventLogFormatError as exc:
                put(name + ".txt", f"decode failed: {exc}\n".encode())
        else:
            put(name + ".json", raw)
    if not bundle.complete:
        why = bundle.truncated or "no trailer"
        put("INCOMPLETE.txt", f"bundle had no complete trailer ({why}): transfer was truncated or aborted\n".encode())
    return written


def fetch_bundle(host: str, timeout: float = 120.0, include_coredump: bool = False) -> bytes:
    url = f"http://{host}/api/diag/bundle" + ("?coredump=1" if include_coredump else "")
    try:
        with http_auth.urlopen(url, timeout=timeout) as resp:
            try:
                data = resp.read(MAX_BUNDLE_BYTES + 1)
            except http.client.IncompleteRead as exc:  # keep what arrived (DB-08)
                data = exc.partial
    except urllib.error.URLError as exc:
        raise BundleError(f"GET {url} failed: {exc}") from exc
    if len(data) > MAX_BUNDLE_BYTES:
        raise BundleError(f"bundle larger than {MAX_BUNDLE_BYTES} bytes")
    return data


def supplement_file_name(path: str) -> Optional[str]:
    """DB-03: file name for a manifest pc_supplement entry, or None when the entry is unsafe."""
    if not isinstance(path, str) or not path.startswith("/api/") or ".." in path:
        return None
    name = "supplement_" + re.sub(r"[^A-Za-z0-9_.\-]", "_", path.strip("/")) + ".json"
    if ".." in name or not _FILE_RE.match(name):
        return None
    return name


def _fetch_supplements(host: str, bundle: Bundle, out_dir: str, timeout: float) -> list[str]:
    written = []
    root = os.path.realpath(out_dir)
    for path in bundle.manifest.get("pc_supplement", []):
        fname = supplement_file_name(path)
        if fname is None:
            continue
        fp = os.path.join(out_dir, fname)
        if os.path.dirname(os.path.realpath(fp)) != root:
            continue
        try:
            with http_auth.urlopen(f"http://{host}{path}", timeout=timeout) as resp:
                body = resp.read()
        except (urllib.error.URLError, OSError) as exc:
            body = json.dumps({"fetch_error": str(exc)}).encode()
        with open(fp, "wb") as f:
            f.write(body)
        written.append(fp)
    return written


def fetch_and_unpack(host: str, out_dir: str, *, elf_paths: Optional[list[str]] = None,
                     symbolize: Optional[Callable[[str, list[str]], str]] = None,
                     timeout: float = 120.0, supplements: bool = True,
                     include_coredump: bool = False) -> list[str]:
    bundle = parse_bundle(fetch_bundle(host, timeout, include_coredump))
    files = unpack_bundle(bundle, out_dir, elf_paths=elf_paths, symbolize=symbolize)
    if supplements:
        files += _fetch_supplements(host, bundle, out_dir, timeout)
    return files
