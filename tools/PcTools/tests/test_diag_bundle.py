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


# ---- DB-03: supplement file names -------------------------------------------------------
@pytest.mark.parametrize("path", ["/api/..\\..\\..\\Users\\x\\evil", "/api/../etc/passwd", "/etc/passwd", "C:\\x", 5, "/api/a/../b"])
def test_supplement_name_rejects_traversal(path):
    assert diag_bundle.supplement_file_name(path) is None


def test_supplement_name_sanitizes_chars():
    n = diag_bundle.supplement_file_name("/api/ota/esp/status?x=1&y=\\z")
    assert n is not None and n.startswith("supplement_") and "/" not in n and "\\" not in n and "?" not in n


def test_fetch_supplements_never_leaves_out_dir(tmp_path, monkeypatch):
    out = tmp_path / "out"
    out.mkdir()
    seen = []

    class R:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self):
            return b"{}"

    monkeypatch.setattr(diag_bundle.http_auth, "urlopen", lambda url, timeout=0: (seen.append(url), R())[1])
    b = diag_bundle.Bundle(manifest={"pc_supplement": ["/api/..\\..\\evil", "/api/status", "/api/a/../b"]}, trailer=None)
    files = diag_bundle._fetch_supplements("h", b, str(out), 1.0)
    assert [os.path.basename(f) for f in files] == ["supplement_api_status.json"]
    assert seen == ["http://h/api/status"]
    assert not list(tmp_path.glob("*.json"))


def test_unpack_put_refuses_unsafe_names(tmp_path):
    b = diag_bundle.Bundle(manifest={}, trailer={"complete": True}, sections={"a..b": b"x"})
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.unpack_bundle(b, str(tmp_path))


# ---- DB-04: frame lengths --------------------------------------------------------------
@pytest.mark.parametrize("ln", [b"-3", b"+3", b"1_0", b" 3", b"00000000000", b"3x", b""])
def test_bad_length_forms_rejected(ln):
    data = frame("_manifest", b"{}") + b"KDF1 a " + ln + b"\nabc\n"
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(data)


def test_negative_length_does_not_hang():
    data = frame("_manifest", b"{}") + b"KDF1 a -1\n\n"
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(data)


def test_oversized_length_rejected():
    data = frame("_manifest", b"{}") + b"KDF1 a 99999999999\n"
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(data)
    data = frame("_manifest", b"{}") + b"KDF1 a 2000000\nabc"
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(data)


# ---- DB-07: reserved names -------------------------------------------------------------
@pytest.mark.parametrize("name", ["manifest", "trailer", "supplement_api_status", "coredump.symbolize", ".hidden", "_other"])
def test_reserved_section_names_rejected(name):
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(build([(name, b"x")]))


# ---- DB-08: ordering, byte counts, truncation ------------------------------------------
def test_manifest_must_be_first():
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(frame("build", b"{}") + frame("_manifest", b"{}"))


def test_duplicate_manifest_rejected():
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(build([("a", b"x")], complete=False) + frame("_manifest", b"{}"))


def test_frames_after_trailer_rejected():
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(build([("a", b"x")]) + frame("b", b"y"))


def test_trailer_byte_count_mismatch_rejected():
    data = frame("_manifest", b"{}") + frame("a", b"abc") + frame(
        "_trailer", json.dumps({"results": [{"name": "a", "bytes": 4}], "complete": True}).encode())
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(data)


def test_trailer_byte_count_match_ok():
    data = frame("_manifest", b"{}") + frame("a", b"ab") + frame("a", b"c") + frame(
        "_trailer", json.dumps({"results": [{"name": "a", "bytes": 3}], "complete": True}).encode())
    assert diag_bundle.parse_bundle(data).complete


def test_bad_json_is_bundle_error():
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.parse_bundle(frame("_manifest", b"{nope"))


def test_mid_frame_truncation_keeps_complete_frames(tmp_path):
    data = build([("build", b"{}"), ("firing_log", b"x" * 100)], complete=False)
    cut = data[:-50]
    b = diag_bundle.parse_bundle(cut)
    assert not b.complete and b.truncated
    assert b.sections == {"build": b"{}"}
    files = diag_bundle.unpack_bundle(b, str(tmp_path))
    names = {os.path.basename(f) for f in files}
    assert {"manifest.json", "build.json", "INCOMPLETE.txt"} <= names
    assert "truncated frame" in (tmp_path / "INCOMPLETE.txt").read_text()


def test_truncated_header_keeps_earlier_frames():
    b = diag_bundle.parse_bundle(build([("build", b"{}")], complete=False) + b"KDF1 fir")
    assert b.sections == {"build": b"{}"} and b.truncated


def test_fetch_bundle_size_cap_and_partial(monkeypatch):
    import http.client

    class R:
        def __init__(self, fn):
            self.fn = fn

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self, n=-1):
            return self.fn(n)

    monkeypatch.setattr(diag_bundle, "MAX_BUNDLE_BYTES", 10)
    monkeypatch.setattr(diag_bundle.http_auth, "urlopen", lambda url, timeout=0: R(lambda n: b"x" * n))
    with pytest.raises(diag_bundle.BundleError):
        diag_bundle.fetch_bundle("h")

    def partial(n):
        raise http.client.IncompleteRead(b"KDF1 ")

    monkeypatch.setattr(diag_bundle.http_auth, "urlopen", lambda url, timeout=0: R(partial))
    assert diag_bundle.fetch_bundle("h") == b"KDF1 "


def test_fetch_bundle_coredump_flag(monkeypatch):
    urls = []

    class R:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self, n=-1):
            return b""

    monkeypatch.setattr(diag_bundle.http_auth, "urlopen", lambda url, timeout=0: (urls.append(url), R())[1])
    diag_bundle.fetch_bundle("h")
    diag_bundle.fetch_bundle("h", include_coredump=True)
    assert urls == ["http://h/api/diag/bundle", "http://h/api/diag/bundle?coredump=1"]


def _core_bundle(sha):
    crash = json.dumps({"present": True, "dump_elf_sha": sha}).encode()
    return diag_bundle.parse_bundle(build([("crash_report", crash), ("coredump", b"\x00core")]))


def test_missing_elf_reported_by_sha(tmp_path):
    b = _core_bundle("abcdef123456")
    diag_bundle.unpack_bundle(b, str(tmp_path), elf_paths=["x.elf"], symbolize=lambda c, e: "ok",
                              find_by_sha=lambda s: (None, f"no archived ELF has sha256 prefix {s}"))
    txt = (tmp_path / "coredump.symbolize.txt").read_text()
    assert "ELF MISSING" in txt and "abcdef123456" in txt


def test_matching_elf_symbolizes(tmp_path):
    b = _core_bundle("abcdef123456")
    diag_bundle.unpack_bundle(b, str(tmp_path), elf_paths=["x.elf"], symbolize=lambda c, e: "decoded-ok",
                              find_by_sha=lambda s: ("x.elf", "found"))
    assert (tmp_path / "coredump.symbolize.txt").read_text() == "decoded-ok"


def test_empty_archive_not_silent(tmp_path):
    b = _core_bundle("abcdef123456")
    diag_bundle.unpack_bundle(b, str(tmp_path), elf_paths=[], symbolize=lambda c, e: "ok")
    assert "NOT SYMBOLIZED" in (tmp_path / "coredump.symbolize.txt").read_text()


def test_release_elf_archive_script_fails_on_bad_input(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "archive_release_elf", os.path.join(os.path.dirname(__file__), "..", "..", "archive_release_elf.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    assert mod.main(["x", str(tmp_path / "no.elf"), str(tmp_path / "no.bin"), "abc", "v1"]) == 1
    assert mod.main(["x"]) == 2
