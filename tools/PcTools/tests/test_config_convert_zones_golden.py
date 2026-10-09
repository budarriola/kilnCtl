"""test_config_convert_zones_golden.py -- pins config_convert.py's
hand-derived zones_cfg_t layout against bytes the FIRMWARE produced.

The fixture tests/fixtures/config_convert/zones_cfg_golden.txt is generated
by firmware/KilnFW/App/test/test_zones_blob_golden.c (part of the KilnFW
zones host-test executable): it fills a real zones_cfg_t with a distinct
sentinel in every field, runs the real nvs_save() (which stamps the CRC and
writes the blob to the fake NVS), and records every field's offset/size/
value plus the raw blob. That C test fails if the committed fixture drifts
from what the current firmware struct produces, so a firmware layout change
turns this pair red rather than letting the Python mirror silently go stale.

This test then decodes that firmware-written blob with
config_convert.decode_zones_blob() (which also verifies the firmware CRC),
checks every single field against the firmware's own value, checks the
decoded field set equals the firmware's field set exactly, and checks
encode_zones_blob() reproduces the firmware bytes exactly.
"""
import re
import struct
from pathlib import Path

import pytest

from kilnctrl import config_convert as cc

GOLDEN = Path(__file__).parent / "fixtures" / "config_convert" / "zones_cfg_golden.txt"

_PATH_TOKEN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?")


def _load_golden():
    fields = []
    meta = {}
    for raw in GOLDEN.read_text(encoding="ascii").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kind, rest = line.split(" ", 1)
        if kind in ("f", "u", "s"):
            path, off, size, value = rest.split(" ", 3)
            if kind == "f":
                value = float(value)
            elif kind == "u":
                value = int(value, 0)
            else:
                assert value.startswith('"') and value.endswith('"'), line
                value = value[1:-1]
            fields.append((kind, path, int(off), int(size), value))
        elif kind == "sizeof":
            name, n = rest.split(" ")
            meta["sizeof " + name] = int(n)
        elif kind == "crc32":
            off, value = rest.split(" ")
            meta["crc32"] = (int(off), int(value, 16))
        elif kind == "blob":
            meta["blob"] = bytes.fromhex(rest)
        else:
            meta[kind] = int(rest)
    return fields, meta


def _lookup(obj, path):
    for m in _PATH_TOKEN.finditer(path):
        obj = obj[m.group(1)]
        if m.group(2) is not None:
            obj = obj[int(m.group(2))]
    return obj


def _leaf_paths(obj, prefix=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from _leaf_paths(v, f"{prefix}.{k}" if prefix else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from _leaf_paths(v, f"{prefix}[{i}]")
    else:
        yield prefix


@pytest.fixture(scope="module")
def golden():
    assert GOLDEN.is_file(), f"missing firmware-generated golden {GOLDEN}"
    return _load_golden()


def test_golden_meta_matches_python_constants(golden):
    _, meta = golden
    assert meta["version"] == cc.ZONES_CFG_VERSION
    assert meta["sizeof zones_cfg_t"] == cc._ZONES_CFG_TOTAL_SIZE
    assert meta["sizeof zone_cfg_t"] == cc._ZONE_CFG_STRUCT.size
    assert meta["sizeof zone_timing_profile_t"] == cc._TIMING_PROFILE_STRUCT.size
    assert len(meta["blob"]) == cc._ZONES_CFG_TOTAL_SIZE
    crc_off, crc = meta["crc32"]
    assert crc_off == cc._ZONES_CFG_TOTAL_SIZE - 4
    assert struct.unpack_from("<I", meta["blob"], crc_off)[0] == crc


def test_decode_matches_every_firmware_field(golden):
    fields, meta = golden
    version, decoded = cc.decode_zones_blob(meta["blob"])  # also verifies the firmware CRC
    assert version == cc.ZONES_CFG_VERSION
    mismatches = []
    for kind, path, _off, _size, want in fields:
        got = _lookup(decoded, path)
        if kind == "f":
            # Every sentinel is exactly representable as float32 (k+0.25), so
            # an exact compare is correct; a shifted field reads a different
            # sentinel, never a near-miss.
            ok = isinstance(got, float) and got == want
        else:
            ok = got == want
        if not ok:
            mismatches.append(f"{path}: firmware {want!r}, config_convert decoded {got!r}")
    assert not mismatches, "layout drift vs firmware:\n" + "\n".join(mismatches)


def test_decoded_field_set_equals_firmware_field_set(golden):
    fields, meta = golden
    _, decoded = cc.decode_zones_blob(meta["blob"])
    firmware_paths = {p for _k, p, _o, _s, _v in fields}
    python_paths = set(_leaf_paths(decoded))
    assert python_paths - firmware_paths == set(), "Python decodes fields the firmware struct lacks"
    assert firmware_paths - python_paths == set(), "firmware struct has fields Python never decodes"


def test_field_offsets_cover_raw_bytes(golden):
    """Each field's own bytes, read at the FIRMWARE's offsetof(), must hold
    the recorded value -- pins the fixture itself as internally consistent."""
    fields, meta = golden
    blob = meta["blob"]
    for kind, path, off, size, want in fields:
        raw = blob[off: off + size]
        if kind == "f":
            assert size == 4 and struct.unpack("<f", raw)[0] == want, path
        elif kind == "u":
            assert int.from_bytes(raw, "little") == want, path
        else:
            assert raw.split(b"\x00", 1)[0].decode("ascii") == want, path


def test_encode_reproduces_firmware_bytes(golden):
    _, meta = golden
    _, decoded = cc.decode_zones_blob(meta["blob"])
    assert cc.encode_zones_blob(decoded) == meta["blob"]
