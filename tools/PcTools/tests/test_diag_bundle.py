"""FD-01: PC-side parse/unpack of the support bundle (firmware: diag_bundle.c)."""
import json
import os
import struct

import pytest

from kilnctrl import diag_bundle, event_log_decoder


def frame(name: str, payload: bytes) -> bytes:
    return b"KDF1 " + name.encode() + b" " + str(len(payload)).encode() + b"\n" + payload + b"\n"


def make_record() -> bytes:
    rec = bytearray(event_log_decoder.RECORD_SIZE)
    rec[0] = event_log_decoder.EVENT_MAGIC
    rec[1] = event_log_decoder.EVENT_VERSION
    return bytes(rec)


def build(sections, complete=True) -> bytes:
    manifest = {"format": "KDIAG1", "sections": [n for n, _ in sections], "pc_supplement": []}
    out = frame("_manifest", json.dumps(manifest).encode())
    for n, p in sections:
        out += frame(n, p)
    if complete:
        out += frame("_trailer", json.dumps({"results": [], "complete": True}).encode())
    return out


def test_parse_multi_frame_concat():
    b = diag_bundle.parse_bundle(build([("alpha", b"abc"), ("alpha", b"de")]))
    assert b.sections["alpha"] == b"abcde"
    assert b.complete


def test_truncated_transfer_detected(tmp_path):
    b = diag_bundle.parse_bundle(build([("build", b"{}")], complete=False))
    assert not b.complete
    files = diag_bundle.unpack_bundle(b, str(tmp_path))
    assert any(f.endswith("INCOMPLETE.txt") for f in files)


@pytest.mark.parametrize("data", [b"KDF1 x 5\nab", b"junk\n", b"KDF1 Bad_Name 1\na\n"])
def test_malformed_rejected(data):
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(data)


def test_unpack_decodes_event_logs_and_symbolizes(tmp_path):
    rec = make_record()
    data = build([("firing_log", rec), ("coredump", b"\x01\x02"), ("build", b"{\"valid\":true}")])
    calls = []

    def sym(path, elves):
        calls.append((path, elves))
        return "frame #0"

    files = diag_bundle.unpack_bundle(diag_bundle.parse_bundle(data), str(tmp_path),
                                      elf_paths=["a.elf"], symbolize=sym)
    names = {os.path.basename(f) for f in files}
    assert {"firing_log.bin", "firing_log.txt", "coredump.bin", "coredump.symbolize.txt", "build.json"} <= names
    assert calls and calls[0][1] == ["a.elf"]
    assert (tmp_path / "firing_log.txt").read_text().strip() != ""


def test_symbolize_failure_is_recorded_not_raised(tmp_path):
    def boom(path, elves):
        raise RuntimeError("no elf")

    diag_bundle.unpack_bundle(diag_bundle.parse_bundle(build([("coredump", b"x")])), str(tmp_path),
                              elf_paths=["a.elf"], symbolize=boom)
    assert "no elf" in (tmp_path / "coredump.symbolize.txt").read_text()


def test_partial_event_record_is_noted(tmp_path):
    data = build([("autotune_log", make_record() + b"xx")])
    diag_bundle.unpack_bundle(diag_bundle.parse_bundle(data), str(tmp_path))
    assert "trailing bytes" in (tmp_path / "autotune_log.txt").read_text()
