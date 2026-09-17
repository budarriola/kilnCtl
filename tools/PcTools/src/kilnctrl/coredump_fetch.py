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

import hashlib
import json
import logging
import os
import shutil
import subprocess
import sys
import time
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

# This board is an ESP32-S3 (see CLAUDE.md/hardware docs) -- espcoredump.py
# needs --chip to pick the right register layout for a raw (non-ELF) core
# file; there is exactly one target in this repo so this is not made
# configurable per call.
ESP_IDF_CHIP_TARGET = "esp32s3"


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


def _resolve_espcoredump_python(espcoredump_python: Optional[str]) -> str:
    """Picks the interpreter to run espcoredump.py with.

    2026-09-16 finding: the MCP server's own venv (`sys.executable`, this
    module's previous unconditional default) does not have `esp_coredump`
    installed -- only the ESP-IDF-provisioned environment
    (`IDF_PYTHON_ENV_PATH`, set by `export.ps1`/`export.sh`) does. Running
    against the wrong interpreter fails with a bare `ModuleNotFoundError`
    that a naive non-zero-exit check then mislabels as "likely an ELF/
    coredump mismatch" -- confirmed live against the board's real stored
    coredump, where the wrong-interpreter failure and a genuine SHA256
    mismatch produced visually similar "exit 1, look at stderr" results
    until stderr was actually read. `_run_espcoredump` below inspects
    stderr for this specific signature and raises a distinct, correctly
    diagnosed error instead of conflating the two.

    Preference order: an explicit `espcoredump_python` argument, then
    `IDF_PYTHON_ENV_PATH`'s interpreter (Windows: Scripts/python.exe, POSIX:
    bin/python) if that env var is set and the file exists, else
    `sys.executable` as a last resort (better than refusing outright when
    IDF_PYTHON_ENV_PATH is unset, since some environments do install
    esp_coredump into the ambient interpreter)."""
    if espcoredump_python:
        return espcoredump_python
    env_root = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_root:
        candidate = os.path.join(env_root, "Scripts", "python.exe")
        if not os.path.isfile(candidate):
            candidate = os.path.join(env_root, "bin", "python")
        if os.path.isfile(candidate):
            return candidate
    return sys.executable


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

    python = _resolve_espcoredump_python(espcoredump_python)
    # NOTE (2026-09-16 fix): `info_corefile`/`dbg_corefile` take the coredump
    # file via the `--core`/`-c` + `--core-format`/`-t` FLAGS, not a bare
    # positional -- the subcommand's only positional argument is `prog` (the
    # ELF). The previous `[subcommand, "-t", "raw", coredump_path, elf_path]`
    # form silently consumed coredump_path as `prog` and left elf_path as an
    # unrecognized trailing argument, which argparse rejects before
    # espcoredump ever inspects either file -- so no real ELF/coredump
    # comparison was ever attempted by this code path. `--chip` is also
    # required (not auto-detected for a raw dump) -- this repo has exactly
    # one target, ESP_IDF_CHIP_TARGET. Confirmed against the real board
    # coredump 2026-09-16: with this corrected invocation espcoredump gets
    # far enough to report a genuine `coredump SHA256(...) != app SHA256(...)`
    # verdict instead of an argparse usage error.
    cmd = [
        python, script, "--chip", ESP_IDF_CHIP_TARGET, subcommand,
        "--core", coredump_path, "--core-format", "raw", elf_path,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CoredumpSymbolizeError(
            f"failed to run espcoredump ({cmd!r}) against elf={elf_path!r} fw_build={fw_build!r}: {exc}"
        ) from exc

    if proc.returncode != 0:
        # Do not conflate "the tool couldn't even run" with "the tool ran and
        # found a mismatch" -- confirmed live that both look like "exit 1"
        # without reading stderr. A missing esp_coredump module says nothing
        # about whether the ELF actually matches the coredump.
        if "ModuleNotFoundError" in proc.stderr and "esp_coredump" in proc.stderr:
            raise CoredumpSymbolizeError(
                f"espcoredump could not run: the interpreter {python!r} does not have the "
                "'esp_coredump' package installed (ModuleNotFoundError), so NO ELF/coredump "
                "comparison was attempted -- this is an environment problem, not a mismatch "
                "verdict. Pass espcoredump_python= pointing at the ESP-IDF-provisioned "
                "interpreter (IDF_PYTHON_ENV_PATH), or `pip install esp-coredump` into it.\n"
                f"--- stderr ---\n{proc.stderr}"
            )
        raise CoredumpSymbolizeError(
            f"espcoredump refused/failed against elf={elf_path!r} fw_build={fw_build!r} "
            f"(exit {proc.returncode}). This is very likely an ELF/coredump mismatch -- do NOT retry "
            f"with a different ELF chosen by guesswork; re-derive it via find_crash_elf().\n"
            f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        )
    return proc.stdout


# Substrings that mark a symbolize failure as an ENVIRONMENT problem (wrong
# interpreter, no IDF_PATH, missing espcoredump.py) rather than a verdict
# about whether a given ELF matches the coredump. find_matching_archived_elf
# below must not swallow these into "this candidate didn't match" -- an
# environment failure says nothing about any candidate and must abort the
# whole search loudly, not be silently absorbed into a false "none of the
# N archived ELFs match" verdict (exactly this repo's recurring failure
# mode: an environment/lookup problem reported as a substantive verdict
# about the data -- see docs/audits/esp_coredump_extraction_2026-09-14.md
# and the 2026-09-16 wrong-interpreter finding above).
_ENVIRONMENT_FAILURE_MARKERS = (
    "ModuleNotFoundError",
    "IDF_PATH is not set",
    "espcoredump.py not found",
    "failed to run espcoredump",
)


def find_matching_archived_elf(coredump_path: str, candidate_elves: list[str], *,
                                espcoredump_python: Optional[str] = None,
                                idf_path: Optional[str] = None,
                                subcommand: str = "info_corefile") -> tuple[str, str]:
    """Finds the ELF that actually produced `coredump_path`, by trying each
    of `candidate_elves` against espcoredump/esp_coredump's own embedded
    SHA256 check (`symbolize_coredump` above) until one succeeds -- rather
    than trusting any externally-reported build identity (a board's
    CURRENTLY RUNNING `fw_build`, a coredump-archive provenance sidecar's
    `fw_build_reported`, or anything else recorded outside the coredump
    itself).

    Why this exists: a coredump persisted on the board outlives the boot
    that wrote it -- a later flash, OTA, or reboot can leave the board
    running a DIFFERENT build by the time the coredump is fetched or
    re-symbolized. `find_crash_elf()`/`read_esp_coredump()`'s original
    behaviour of keying the ELF lookup off the board's *current* `fw_build`
    is confidently wrong in exactly that case: it returns a plausible ELF
    that symbolizes cleanly-looking output for the wrong build, reproducing
    the "confident wrong line numbers" failure this whole module exists to
    prevent. Confirmed against a real stored coredump on the bench board:
    the dump's own embedded app SHA256 began `0965ca575...` while the
    board's then-currently-running app's began `1204ef146...` -- two
    different builds. The embedded SHA256 esp_coredump checks is the
    strongest available identifier of a dump's true origin build, so this
    function verifies against it directly instead of trusting any
    externally-recorded metadata.

    Returns (elf_path, stdout) for the first candidate that satisfies
    esp_coredump's own SHA256 check. Raises `CoredumpSymbolizeError`:
      - immediately, naming the underlying problem, if any candidate
        attempt fails for an ENVIRONMENT reason (wrong interpreter,
        IDF_PATH unset, espcoredump.py missing) -- never conflated with a
        "no match" verdict, since an environment failure says nothing about
        whether any candidate's content actually matches;
      - after exhausting every candidate, if all of them fail on content
        (an actual SHA256 mismatch each time) -- worded as a PERMANENTLY
        UNSYMBOLIZABLE dump (the build that produced it was never archived,
        or its ELF has since been pruned), a legitimate, distinctly-labeled
        outcome, not an error that reads like an environment problem or a
        single wrong guess."""
    if not candidate_elves:
        raise CoredumpSymbolizeError(
            "no archived ELF candidates were given to search -- nothing to try. "
            "This means the ELF archive is empty, not that this coredump is unsymbolizable."
        )
    tried: list[tuple[str, str]] = []
    for elf_path in candidate_elves:
        try:
            out = symbolize_coredump(
                coredump_path, elf_path,
                fw_build=f"<candidate {os.path.basename(elf_path)}, content-based search>",
                espcoredump_python=espcoredump_python, idf_path=idf_path, subcommand=subcommand,
            )
        except CoredumpSymbolizeError as exc:
            msg = str(exc)
            if any(marker in msg for marker in _ENVIRONMENT_FAILURE_MARKERS):
                # Do not keep trying other candidates against a broken
                # toolchain -- every subsequent attempt would fail the same
                # way for the same non-content reason, and reporting that as
                # "N/N candidates didn't match" would be exactly the
                # environment-failure-reported-as-a-data-verdict bug this
                # function exists to avoid.
                raise
            tried.append((elf_path, msg.splitlines()[-1] if msg else "<empty>"))
            continue
        return elf_path, out
    detail = "\n".join(f"  - {p}: {m}" for p, m in tried)
    raise CoredumpSymbolizeError(
        f"cannot symbolize: PERMANENTLY UNSYMBOLIZABLE -- none of the {len(candidate_elves)} archived "
        "ELF(s) checked match this coredump's own embedded build identity (each failed esp_coredump's "
        "SHA256 check). This means the build that actually produced this coredump was never archived, "
        "or its ELF has since been pruned -- a legitimate outcome, not a tooling bug. Do NOT retry with "
        "a guessed substitute ELF; that reproduces the exact confident-wrong-line-numbers failure this "
        "module exists to prevent.\n" + detail
    )


# ---------------------------------------------------------------------------
# Durable, provenance-carrying archive of every raw coredump ever fetched.
#
# Why this exists (2026-09-16, RELEASE_HARDENING_PLAN.md blocker 1 item 1):
# `read_esp_coredump()` used to fetch into a single fixed path
# (`coredump_<host>.bin` under the OS temp dir), overwritten on every call
# with no record of which board/build/moment it came from. That is fine for
# an interactive "look at it right now" session but loses the evidence the
# instant a second fetch happens, and is exactly the kind of silent
# overwrite this repo's "reset one side of a pair" and "unregistered
# producer" bug classes (see elf_archive.py, CLAUDE.md) warn about: a fetch
# is a one-shot opportunity to capture what is on the board (a later flash
# or an operator running `/api/crash_report/clear` -- never done by this
# module -- can make the same board unreadable), so losing the copy by
# overwriting it is a real cost, not a convenience trade.
#
# `archive_coredump()` below copies the already-fetched raw dump into a
# permanent, uniquely-named location and writes a sidecar JSON naming the
# board (host), the fw_build it reported AT FETCH TIME (which, per the
# 2026-09-16 finding above, is NOT guaranteed to be the build that actually
# produced the dump -- a stored coredump outlives the boot/flash that wrote
# it, so this field records "what was running when we fetched it", not "what
# panicked"; a caller with a fw_build read from the crash_report at panic
# time should pass THAT as fw_build_reported instead), when it was fetched,
# and the dump's own sha256 (so two fetches of the same still-unacknowledged
# dump are cheap to recognize as identical rather than being mistaken for
# two different incidents). Nothing in this module ever deletes or
# overwrites an existing archived entry -- there is no prune/cap here at
# all, unlike elf_archive.py, because coredumps are rare (one board panic at
# a time, at most) rather than one-per-build.
# ---------------------------------------------------------------------------


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/ -> repo root is four levels up. Kept as a
    private copy (not imported from elf_archive.py) so this module stays
    usable/testable without importing elf_archive at all."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


def coredump_archive_dir() -> str:
    """Canonical durable coredump archive -- a sibling of build/ and of
    elf_archive/, never inside build/, for the same reason elf_archive.py's
    kiln_archive_dir() moved out of build/ on 2026-09-15: anything inside
    build/ can be silently wiped by `idf.py fullclean` or an equivalent
    fresh-configure, which is exactly what destroyed the ELF this module's
    own 2026-09-16 investigation needed (see docs/RELEASE_HARDENING_PLAN.md
    blocker 1 and the audit trail it cites)."""
    return os.path.join(_repo_root(), "firmware", "KilnFW", "coredump_archive")


@dataclass
class ArchivedCoredump:
    path: str
    provenance_path: str
    sha256: str
    byte_len: int


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def archive_coredump(fetched_path: str, *, host: str, fw_build_reported: Optional[str],
                      archive_dir: Optional[str] = None) -> ArchivedCoredump:
    """Copies `fetched_path` (a coredump already pulled by
    `fetch_coredump_over_http()`) into the durable archive
    (`coredump_archive_dir()` by default) under a name keyed by the dump's
    own content hash, and writes a `<name>.json` provenance sidecar
    recording `host`, `fw_build_reported` (the board's `/api/status` fw_build
    AT FETCH TIME -- see the module-level note above on why that is not
    necessarily the build that produced the panic), `fetched_at` (UTC,
    `%Y-%m-%dT%H:%M:%SZ`), `sha256`, and `byte_len`.

    Never overwrites an existing archived file: if the content hash already
    has an entry (the same still-unacknowledged dump fetched twice), this
    returns the EXISTING entry unchanged rather than writing a duplicate --
    idempotent, and it never deletes anything either way. Raises
    `CoredumpFetchError` (not silently returning a half-written pair) if the
    source file is missing or the destination directory cannot be created."""
    if not os.path.isfile(fetched_path):
        raise CoredumpFetchError(f"cannot archive: fetched coredump does not exist: {fetched_path!r}")
    dest_dir = archive_dir or coredump_archive_dir()
    try:
        os.makedirs(dest_dir, exist_ok=True)
    except OSError as exc:
        raise CoredumpFetchError(f"cannot create coredump archive dir {dest_dir!r}: {exc}") from exc

    digest = _sha256_file(fetched_path)
    short = digest[:12]
    dest_path = os.path.join(dest_dir, f"coredump-{short}.bin")
    prov_path = os.path.join(dest_dir, f"coredump-{short}.json")

    if os.path.isfile(dest_path) and os.path.isfile(prov_path):
        # Same content already archived -- do not overwrite the earlier
        # provenance record (it may name an earlier, still-accurate fetch
        # time) and do not write a second copy of identical bytes.
        with open(prov_path, "r", encoding="utf-8") as f:
            prior = json.load(f)
        return ArchivedCoredump(path=dest_path, provenance_path=prov_path,
                                 sha256=digest, byte_len=int(prior.get("byte_len", os.path.getsize(dest_path))))

    byte_len = os.path.getsize(fetched_path)
    provenance = {
        "host": host,
        "fw_build_reported": fw_build_reported,
        "fetched_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "sha256": digest,
        "byte_len": byte_len,
        "source_path": os.path.abspath(fetched_path),
    }
    # Copy first, then write the sidecar, so a crash between the two never
    # leaves a provenance record with no backing file -- a dangling .bin
    # with no .json is recoverable (re-hash it); a .json with no .bin is not.
    tmp_dest = dest_path + ".part"
    shutil.copyfile(fetched_path, tmp_dest)
    os.replace(tmp_dest, dest_path)
    with open(prov_path, "w", encoding="utf-8") as f:
        json.dump(provenance, f, indent=2)
    logger.info("archived coredump: %s (%d bytes, sha256=%s) -> %s", host, byte_len, short, dest_path)
    return ArchivedCoredump(path=dest_path, provenance_path=prov_path, sha256=digest, byte_len=byte_len)
