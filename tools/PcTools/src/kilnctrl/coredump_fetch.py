"""coredump_fetch.py -- pulls the ESP's coredump partition off the board over
HTTP (diagnostics_http.c's `/api/coredump/info` + `/api/coredump/chunk`,
2026-09-14) and hands it to ESP-IDF's `espcoredump.py` for symbolization,
matched against the archived ELF `find_kiln_elf_for_build`/`elf_archive.py`
already track.

Why this exists: docs/audits/esp_coredump_extraction_2026-09-14.md -- an
agent investigating a `profile_executor`/`IllegalInstruction` panic ran what
it believed was a read-only OpenOCD flash-bank query against the live board;
loading and running an on-target flasher stub is NOT read-only on the
ESP32-S3, and the resulting stall tripped the interrupt watchdog and reset
the board, risking the very coredump it was trying to read. This module is
the durable fix that incident report calls for: a way to get the coredump
off the board that touches neither JTAG nor the CPU's own execution state --
plain HTTP GETs against a running board, the same class of call as
`get_heap_status()`.

Two failure classes this module is built to never paper over, per the
2026-09-14 task's explicit requirement -- a tool that emits a plausible-
looking WRONG backtrace is worse than one that refuses outright, and this
repo has already been misled by symbolizing against the wrong ELF twice:
  - no coredump present on the board at all (`/api/coredump/info` reports
    `present: false`, or `data_len` reads back as the BLANK sentinel);
  - the archived ELF `find_kiln_elf_for_build()` returns does not actually
    match the coredump being symbolized. `espcoredump.py`/`esp_coredump`
    itself already refuses on an ELF/coredump SHA256 mismatch (embedded in
    the coredump's own header) -- `symbolize_coredump()` below does not
    swallow that refusal or downgrade it to a warning; it re-raises loudly
    with both identities named.

Both fetch and symbolize are separated so `fetch_coredump_over_http()` alone
is usable read-only diagnostics (confirm presence/size without needing a
toolchain installed), and so `symbolize_coredump()`'s ELF-matching logic is
unit-testable without a live board (see test_coredump_fetch.py's mocked
subprocess mismatch case).
"""
from __future__ import annotations

import json
import logging
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Optional

logger = logging.getLogger(__name__)

# Must match diagnostics_http.c's COREDUMP_HTTP_CHUNK_MAX -- kept as a
# separate constant (not imported, there is nothing to import from C) but
# named identically so a future change to one is easy to spot missing its
# twin. A caller may ask for less; more is clamped board-side regardless, so
# this value is a request-side default, not a limit the board trusts blindly.
COREDUMP_HTTP_CHUNK_BYTES = 4096

# BLANK_COREDUMP_SIZE from esp-idf/components/espcoredump/src/core_dump_flash.c --
# what `data_len` reads back as when no coredump has ever been written.
COREDUMP_BLANK_LEN = 0xFFFFFFFF


class CoredumpFetchError(RuntimeError):
    """Raised for every failure this module can detect itself -- no coredump
    present, a truncated/short transfer, or an HTTP error -- so a caller
    cannot mistake a partial or absent result for a valid one."""


class CoredumpSymbolizeError(RuntimeError):
    """Raised when the ELF handed to espcoredump does not match the
    coredump (or espcoredump/esp_coredump itself is not installed, or the
    subprocess otherwise fails). Always carries the ELF path and fw_build
    identity that were used, so the failure names what was tried."""


@dataclass
class CoredumpInfo:
    present: bool
    data_len: int
    partition_size: int
    chunk_size: int

    @property
    def blank(self) -> bool:
        return self.data_len == COREDUMP_BLANK_LEN


def _http_get_json(url: str, timeout: float) -> dict:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            body = resp.read()
    except urllib.error.URLError as exc:
        raise CoredumpFetchError(f"GET {url} failed: {exc}") from exc
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise CoredumpFetchError(f"GET {url} returned non-JSON body: {body[:200]!r}") from exc


def _http_get_bytes(url: str, timeout: float) -> bytes:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return resp.read()
    except urllib.error.URLError as exc:
        raise CoredumpFetchError(f"GET {url} failed: {exc}") from exc


def get_coredump_info(host: str, timeout: float = 5.0) -> CoredumpInfo:
    """GET /api/coredump/info -- cheap, single round trip. Callers should
    check `.present` and `.blank` before spending time on a full fetch."""
    url = f"http://{host}/api/coredump/info"
    data = _http_get_json(url, timeout)
    if not data.get("ok", False):
        raise CoredumpFetchError(f"{url} reported an error: {data.get('error', '<no error field>')}")
    return CoredumpInfo(
        present=bool(data.get("present", False)),
        data_len=int(data.get("data_len", COREDUMP_BLANK_LEN)),
        partition_size=int(data.get("partition_size", 0)),
        chunk_size=int(data.get("chunk_size", COREDUMP_HTTP_CHUNK_BYTES)),
    )


def fetch_coredump_over_http(host: str, out_path: str, *, chunk_size: Optional[int] = None,
                              timeout: float = 10.0) -> int:
    """Fetches the coredump image (the first `data_len` bytes of the
    `coredump` partition -- NOT the whole ~1 MB partition, just the portion
    espcoredump actually needs) into `out_path`, in bounded chunks over
    `/api/coredump/chunk?offset=N&len=M`. Returns the number of bytes
    written. Raises CoredumpFetchError loudly rather than writing a
    truncated file silently: no coredump present, a blank image, a short
    read (fewer bytes than requested and not yet at data_len), or any HTTP
    failure mid-transfer all abort before the output file is left behind
    half-written (written to a `.part` path, renamed only on full success)."""
    info = get_coredump_info(host, timeout=timeout)
    if not info.present or info.blank:
        raise CoredumpFetchError(
            f"no coredump present on {host} (present={info.present}, data_len=0x{info.data_len:08x}) -- "
            "nothing to fetch. Do not treat this as a tool bug: it may simply mean the board has never "
            "panicked, or the coredump was already cleared via /api/crash_report/clear."
        )
    total = info.data_len
    if total > info.partition_size:
        raise CoredumpFetchError(
            f"coredump data_len (0x{total:08x}) exceeds the partition size (0x{info.partition_size:08x}) "
            "reported by the same /api/coredump/info call -- the image header is not self-consistent, "
            "refusing to fetch a value that cannot be trusted."
        )
    req_chunk = chunk_size or info.chunk_size or COREDUMP_HTTP_CHUNK_BYTES

    part_path = out_path + ".part"
    written = 0
    with open(part_path, "wb") as f:
        offset = 0
        while offset < total:
            this_len = min(req_chunk, total - offset)
            url = f"http://{host}/api/coredump/chunk?offset={offset}&len={this_len}"
            chunk = _http_get_bytes(url, timeout)
            if len(chunk) == 0:
                raise CoredumpFetchError(
                    f"empty chunk at offset {offset}/{total} from {url} -- aborting rather than "
                    "writing a silently-truncated coredump file"
                )
            f.write(chunk)
            written += len(chunk)
            offset += len(chunk)
    if written != total:
        try:
            os.remove(part_path)
        except OSError:
            pass
        raise CoredumpFetchError(
            f"transfer size mismatch: wrote {written} bytes but /api/coredump/info reported "
            f"data_len={total} -- deleted the partial file rather than leaving a truncated "
            "coredump on disk under a name that looks complete"
        )
    os.replace(part_path, out_path)
    logger.info("fetched coredump: %d bytes from %s -> %s", written, host, out_path)
    return written


def symbolize_coredump(coredump_path: str, elf_path: str, *, fw_build: str = "<unspecified>",
                        espcoredump_python: Optional[str] = None,
                        idf_path: Optional[str] = None,
                        subcommand: str = "info_corefile") -> str:
    """Runs ESP-IDF's `espcoredump.py <subcommand>` against `coredump_path`
    with `--rom-elf`/prog `elf_path`, and returns its stdout on success.

    Fails LOUD (`CoredumpSymbolizeError`) rather than returning a plausible-
    looking result on:
      - `elf_path` not existing on disk (caught here, before ever invoking
        espcoredump, so the failure names the missing path instead of
        whatever confusing error espcoredump would produce for it);
      - espcoredump/esp_coredump exiting non-zero for ANY reason, including
        (this is the case that matters) an ELF/coredump SHA256 mismatch --
        esp_coredump's own parser checks this and refuses, and that refusal
        is passed through verbatim in the exception message along with the
        `elf_path`/`fw_build` this call was given, never downgraded to a
        warning or retried against a different ELF automatically. Silently
        falling back to a different file is exactly the failure mode
        (confident wrong line numbers) this whole module exists to prevent.
    """
    if not os.path.isfile(elf_path):
        raise CoredumpSymbolizeError(
            f"ELF path does not exist: {elf_path!r} (fw_build={fw_build!r}) -- "
            "not attempting to symbolize against a file that isn't there"
        )
    if not os.path.isfile(coredump_path):
        raise CoredumpSymbolizeError(f"coredump file does not exist: {coredump_path!r}")

    idf_path = idf_path or os.environ.get("IDF_PATH")
    if not idf_path:
        raise CoredumpSymbolizeError(
            "IDF_PATH is not set and no idf_path was given -- cannot locate espcoredump.py "
            "(see docs/MCP_SERVERS.md for the sanctioned ESP-IDF environment setup)"
        )
    script = os.path.join(idf_path, "components", "espcoredump", "espcoredump.py")
    if not os.path.isfile(script):
        raise CoredumpSymbolizeError(f"espcoredump.py not found at expected path: {script!r}")

    python = espcoredump_python or sys.executable
    cmd = [python, script, subcommand, "-t", "raw", coredump_path, elf_path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CoredumpSymbolizeError(
            f"failed to run espcoredump ({cmd!r}) against elf={elf_path!r} fw_build={fw_build!r}: {exc}"
        ) from exc

    if proc.returncode != 0:
        raise CoredumpSymbolizeError(
            f"espcoredump refused/failed against elf={elf_path!r} fw_build={fw_build!r} "
            f"(exit {proc.returncode}). This is very likely an ELF/coredump mismatch -- do NOT retry "
            f"with a different ELF chosen by guesswork; re-derive it via find_crash_elf().\n"
            f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        )
    return proc.stdout
