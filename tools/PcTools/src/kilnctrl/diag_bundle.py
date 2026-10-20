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

import json
import os
import re
import urllib.error
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import event_log_decoder, http_auth

__all__ = ["BundleError", "Bundle", "parse_bundle", "unpack_bundle", "fetch_bundle", "fetch_and_unpack"]

_NAME_RE = re.compile(r"^[a-z0-9_.\-]{1,31}$")
_EVENT_LOGS = ("firing_log", "autotune_log")


class BundleError(RuntimeError):
    pass


@dataclass
class Bundle:
    manifest: dict
    trailer: Optional[dict]
    sections: dict[str, bytes] = field(default_factory=dict)

    @property
    def complete(self) -> bool:
        return self.trailer is not None and bool(self.trailer.get("complete"))


def parse_bundle(data: bytes) -> Bundle:
    pos = 0
    sections: dict[str, bytes] = {}
    manifest = None
    trailer = None
    while pos < len(data):
        nl = data.find(b"\n", pos)
        if nl < 0:
            raise BundleError(f"truncated frame header at byte {pos}")
        head = data[pos:nl].split(b" ")
        if len(head) != 3 or head[0] != b"KDF1":
            raise BundleError(f"bad frame header at byte {pos}: {data[pos:nl][:40]!r}")
        name = head[1].decode("ascii", "replace")
        if not _NAME_RE.match(name):
            raise BundleError(f"bad section name {name!r}")
        try:
            length = int(head[2])
        except ValueError as exc:
            raise BundleError(f"bad frame length {head[2]!r}") from exc
        start = nl + 1
        end = start + length
        if end + 1 > len(data) or data[end:end + 1] != b"\n":
            raise BundleError(f"truncated or unterminated frame {name!r} at byte {pos}")
        payload = data[start:end]
        pos = end + 1
        if name == "_manifest":
            manifest = json.loads(payload)
        elif name == "_trailer":
            trailer = json.loads(payload)
        else:
            sections[name] = sections.get(name, b"") + payload
    if manifest is None:
        raise BundleError("bundle has no _manifest frame")
    return Bundle(manifest=manifest, trailer=trailer, sections=sections)


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

    def put(name: str, content: bytes) -> str:
        path = os.path.join(out_dir, name)
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
        put("INCOMPLETE.txt", b"bundle had no complete trailer: transfer was truncated or aborted\n")
    return written


def fetch_bundle(host: str, timeout: float = 120.0) -> bytes:
    url = f"http://{host}/api/diag/bundle"
    try:
        with http_auth.urlopen(url, timeout=timeout) as resp:
            return resp.read()
    except urllib.error.URLError as exc:
        raise BundleError(f"GET {url} failed: {exc}") from exc


def _fetch_supplements(host: str, bundle: Bundle, out_dir: str, timeout: float) -> list[str]:
    written = []
    for path in bundle.manifest.get("pc_supplement", []):
        if not isinstance(path, str) or not path.startswith("/api/"):
            continue
        fname = "supplement_" + path.strip("/").replace("/", "_") + ".json"
        try:
            with http_auth.urlopen(f"http://{host}{path}", timeout=timeout) as resp:
                body = resp.read()
        except (urllib.error.URLError, OSError) as exc:
            body = json.dumps({"fetch_error": str(exc)}).encode()
        fp = os.path.join(out_dir, fname)
        with open(fp, "wb") as f:
            f.write(body)
        written.append(fp)
    return written


def fetch_and_unpack(host: str, out_dir: str, *, elf_paths: Optional[list[str]] = None,
                     symbolize: Optional[Callable[[str, list[str]], str]] = None,
                     timeout: float = 120.0, supplements: bool = True) -> list[str]:
    bundle = parse_bundle(fetch_bundle(host, timeout))
    files = unpack_bundle(bundle, out_dir, elf_paths=elf_paths, symbolize=symbolize)
    if supplements:
        files += _fetch_supplements(host, bundle, out_dir, timeout)
    return files
