"""test_config_convert_zones_history.py -- stage 2 of config_convert.py: the
historical zones_cfg_t shapes v21..v25 upgrading to the current layout.

Older blobs are derived from the firmware-generated current-version golden
(zones_cfg_golden.txt) by truncating every zone to the older frozen size and
re-stamping the crc32, then checked against the golden's own field values.
The frozen sizes/offsets this module transcribed are mirror-checked against
the real _Static_asserts in zones_config_json.h.
"""

import re
import struct
import zlib
from pathlib import Path

import pytest

from kilnctrl import config_convert as cc

REPO = Path(__file__).resolve().parents[3]
HEADER = REPO / "firmware" / "KilnFW" / "App" / "drivers" / "persist" / "zones_config_json.h"
GOLDEN = Path(__file__).parent / "fixtures" / "config_convert" / "zones_cfg_golden.txt"

HDR = 8
ZSZ = cc._ZONE_CFG_STRUCT.size


def _golden_blob() -> bytes:
    for line in GOLDEN.read_text(encoding="utf-8").splitlines():
        if line.startswith("blob "):
            return bytes.fromhex(line.split(None, 1)[1].strip())
    raise AssertionError("no blob line in golden")


def _stamp(body: bytes) -> bytes:
    return body + struct.pack("<I", zlib.crc32(body + b"\x00\x00\x00\x00"))


def _downgrade(blob: bytes, version: int) -> bytes:
    """Cut a current-version blob down to `version`'s shape (tail append chain)."""
    zsz = cc._ZONE_SIZE_BY_VERSION[version]
    out = bytes([version]) + blob[1:HDR]
    for i in range(3):
        base = HDR + i * ZSZ
        out += blob[base: base + zsz]
    rest = blob[HDR + 3 * ZSZ: -4]
    return _stamp(out + rest)


def _zone_bytes(blob: bytes, i: int, size: int) -> bytes:
    return blob[HDR + i * size: HDR + i * size + size]


@pytest.fixture(scope="module")
def cur():
    b = _golden_blob()
    assert len(b) == 896 and b[0] == cc.ZONES_CFG_VERSION
    return b


def test_sizes_match_formula():
    for v, z in cc._ZONE_SIZE_BY_VERSION.items():
        assert cc._zones_total_size_for_version(v) == 152 + 3 * z


@pytest.mark.parametrize("version", [21, 22, 23, 24, 25])
def test_old_version_upgrades_and_preserves_prefix_fields(cur, version):
    old = _downgrade(cur, version)
    assert len(old) == 152 + 3 * cc._ZONE_SIZE_BY_VERSION[version]
    src, fields = cc.decode_zones_blob(old)
    assert src == version
    assert fields["version"] == cc.ZONES_CFG_VERSION
    _, cfields = cc.decode_zones_blob(cur)
    carried = [k for k in cfields["zones"][0]
               if k not in cc._zone_added_fields_after(version)
               and k not in ("heater_min_on_ms", "heater_window_ms")]
    for zi in range(3):
        for k in carried:
            assert fields["zones"][zi][k] == cfields["zones"][zi][k], (version, zi, k)
    assert fields["timing_profiles"] == cfields["timing_profiles"]
    assert fields["pc_link_abort_silence_ms"] == cfields["pc_link_abort_silence_ms"]
    for zi in range(3):
        for k in cc._zone_added_fields_after(version):
            if k.startswith("model_fit"):
                continue
            assert fields["zones"][zi][k] == 0, (version, k)


@pytest.mark.parametrize("version", [21, 22, 23])
def test_pre_v24_model_fit_is_unknown(cur, version):
    _, fields = cc.decode_zones_blob(_downgrade(cur, version))
    for z in fields["zones"]:
        assert z["model_fit_temp_c"] == pytest.approx(-273.15, abs=1e-4)
        assert z["model_fit_ambient_c"] == pytest.approx(-273.15, abs=1e-4)


@pytest.mark.parametrize("version", [24, 25])
def test_v24_v25_model_fit_preserved_and_reported(cur, version):
    old = _downgrade(cur, version)
    out, report = cc.convert_zones_blob(old, cc.ZONES_CFG_VERSION)
    _, cfields = cc.decode_zones_blob(cur)
    _, ofields = cc.decode_zones_blob(out)
    for zi in range(3):
        assert ofields["zones"][zi]["model_fit_temp_c"] == cfields["zones"][zi]["model_fit_temp_c"]
    assert any(o.field.startswith("model_fit") and o.action == "kept" for o in report.outcomes)


@pytest.mark.parametrize("version", [21, 22, 23, 24, 25])
def test_convert_output_is_valid_current_blob(cur, version):
    out, report = cc.convert_zones_blob(_downgrade(cur, version), cc.ZONES_CFG_VERSION)
    assert len(out) == 896 and out[0] == cc.ZONES_CFG_VERSION
    assert cc._zones_crc32_n(out) == struct.unpack_from("<I", out, 892)[0]
    assert report.source_version == version and report.target_version == cc.ZONES_CFG_VERSION
    assert cc.decode_zones_blob(out)[0] == cc.ZONES_CFG_VERSION


def test_heater_floors_raised_on_upgrade(cur):
    old = bytearray(_downgrade(cur, 25))
    _, f = cc.decode_zones_blob(bytes(old))
    z0 = f["zones"][0]
    # heater_min_on_ms is a float in the 24-float block; locate by re-encoding.
    z0["heater_min_on_ms"] = 2000.0
    z0["heater_window_ms"] = 4000.0
    f["version"] = cc.ZONES_CFG_VERSION
    raw = cc._encode_zone_cfg(z0)[: cc._ZONE_SIZE_BY_VERSION[25]]
    old[HDR: HDR + len(raw)] = raw
    stamped = _stamp(bytes(old[:-4]))
    out, report = cc.convert_zones_blob(stamped, cc.ZONES_CFG_VERSION)
    _, of = cc.decode_zones_blob(out)
    assert of["zones"][0]["heater_min_on_ms"] == 10000.0
    assert of["zones"][0]["heater_window_ms"] == 30000.0
    assert any(o.field == "heater_min_on_ms" for o in report.outcomes)


# ---- negative cases -------------------------------------------------------

def test_old_version_bad_crc_refused(cur):
    old = bytearray(_downgrade(cur, 24))
    old[40] ^= 0x01
    with pytest.raises(cc.ConfigConvertError, match="crc32"):
        cc.decode_zones_blob(bytes(old))


@pytest.mark.parametrize("version", [21, 25])
def test_old_version_wrong_length_refused(cur, version):
    old = _downgrade(cur, version)
    with pytest.raises(cc.ConfigConvertError, match="bytes, not"):
        cc.decode_zones_blob(old[:-1])
    with pytest.raises(cc.ConfigConvertError, match="bytes, not"):
        cc.decode_zones_blob(old + b"\x00")


def test_version_claiming_wrong_shape_refused(cur):
    # a v25-sized body claiming v24 must not be guessed at
    old = bytearray(_downgrade(cur, 25))
    old[0] = 24
    with pytest.raises(cc.ConfigConvertError, match="bytes, not"):
        cc.decode_zones_blob(bytes(old))


def test_v20_and_below_refused(cur):
    for v in (0, 1, 6, 20):
        with pytest.raises(cc.ConfigConvertError, match="older than"):
            cc.decode_zones_blob(bytes([v]) + bytes(100))


def test_newer_version_refused(cur):
    with pytest.raises(cc.ConfigConvertError, match="newer"):
        cc.decode_zones_blob(bytes([cc.ZONES_CFG_VERSION + 1]) + bytes(100))


def test_truncated_blob_refused():
    with pytest.raises(cc.ConfigConvertError):
        cc.decode_zones_blob(b"")
    with pytest.raises(cc.ConfigConvertError):
        cc.decode_zones_blob(bytes([23]) + bytes(10))


def test_kiln_package_with_old_blob_converts(cur):
    old = _downgrade(cur, 24)
    pico = []
    doc = {
        "kind": "kilnctl_kiln_package", "pkg_schema": 1, "esp_blob_hex": old.hex(),
        "esp_blob_len": len(old), "pico": pico, "source_board_id": "x",
        "pkg_hash": f"0x{cc._kiln_pkg_compute_hash(1, old, pico):08x}",
    }
    out, report = cc.convert_document(doc, cc.ZONES_CFG_VERSION)
    new_blob = bytes.fromhex(out["esp_blob_hex"])
    assert len(new_blob) == 896 and report.source_version == 24
    assert int(out["pkg_hash"], 16) == cc._kiln_pkg_compute_hash(1, new_blob, pico)


# ---- kiln_configs[] inside a backup document -------------------------------

def _pkg_doc(blob: bytes, pico=None, name="slot"):
    pico = pico if pico is not None else []
    return {
        "kind": "kilnctl_kiln_package", "pkg_schema": 1, "name": name, "esp_blob_hex": blob.hex(),
        "esp_blob_len": len(blob), "pico": pico, "source_board_id": "x",
        "pkg_hash": f"0x{cc._kiln_pkg_compute_hash(1, blob, pico):08x}",
    }


def _backup_with(entries):
    return {"kind": "kilnctl_backup", "version": 5, "profiles": [], "zones": [], "kiln_configs": entries}


def test_backup_kiln_configs_two_packages_both_converted(cur):
    old21 = _downgrade(cur, 21)
    old24 = _downgrade(cur, 24)
    doc = _backup_with([
        {"id": 1, "name": "a", "is_active": True, "package": _pkg_doc(old21, name="a")},
        {"id": 2, "name": "b", "is_active": False, "package": _pkg_doc(old24, name="b")},
    ])
    out, report = cc.convert_document(doc, 5)
    assert not report.failed
    assert len(out["kiln_configs"]) == 2
    for entry, want_src in zip(out["kiln_configs"], (21, 24)):
        blob = bytes.fromhex(entry["package"]["esp_blob_hex"])
        assert len(blob) == 896 and blob[0] == cc.ZONES_CFG_VERSION
        assert int(entry["package"]["pkg_hash"], 16) == cc._kiln_pkg_compute_hash(1, blob, [])
        assert entry["is_active"] in (True, False)
    # the unconverted input is untouched (no aliasing)
    assert doc["kiln_configs"][0]["package"]["esp_blob_hex"] == old21.hex()
    detail = " ".join(o.detail for o in report.outcomes if o.field == "package")
    assert "v21 -> v" in detail and "v24 -> v" in detail
    assert not any(o.field == "kiln_configs" and o.action == "dropped" for o in report.outcomes)


def test_backup_kiln_configs_bad_slot_reported_and_kept_not_dropped(cur):
    good = _pkg_doc(_downgrade(cur, 24), name="good")
    bad = _pkg_doc(_downgrade(cur, 24), name="bad")
    bad["pkg_hash"] = "0xdeadbeef"  # tampered: fails its own integrity check
    doc = _backup_with([
        {"id": 1, "name": "good", "package": good},
        {"id": 2, "name": "bad", "package": bad},
        {"id": 3, "name": "legacy", "omitted": "no_pico_half"},
    ])
    out, report = cc.convert_document(doc, 5)
    assert report.failed
    assert len(out["kiln_configs"]) == 3  # nothing dropped
    assert bytes.fromhex(out["kiln_configs"][0]["package"]["esp_blob_hex"])[0] == cc.ZONES_CFG_VERSION
    assert out["kiln_configs"][1] == doc["kiln_configs"][1]  # failed slot carried through unchanged
    assert out["kiln_configs"][2] == doc["kiln_configs"][2]
    failed = [o for o in report.outcomes if o.action == "failed"]
    assert len(failed) == 1 and "kiln_configs[1]" in failed[0].scope and "pkg_hash" in failed[0].detail
    assert report.as_dict()["failed"] is True


def test_backup_kiln_configs_main_exits_nonzero_on_failed_slot(cur, tmp_path):
    bad = _pkg_doc(_downgrade(cur, 24))
    bad["pkg_hash"] = "0x1"
    src = tmp_path / "b.json"
    src.write_text(__import__("json").dumps(_backup_with([{"id": 1, "name": "x", "package": bad}])))
    dst = tmp_path / "o.json"
    assert cc.main([str(src), "--to-version", "5", "-o", str(dst), "--quiet"]) == 2
    assert dst.exists()  # output still written; the failed slot is in it unchanged


def test_backup_kiln_configs_dropped_for_target_older_than_v5(cur):
    """A v4 target predates kiln_configs[]: the slots are reported dropped and
    left out, never re-added to a document an older firmware reads."""
    doc = _backup_with([{"id": 1, "name": "a", "package": _pkg_doc(_downgrade(cur, 24), name="a")}])
    out, report = cc.convert_document(doc, 4)
    assert "kiln_configs" not in out
    assert any(o.field == "kiln_configs" and o.action == "dropped" for o in report.outcomes)
    assert not report.failed


# ---- mirror of the firmware's frozen structs -------------------------------

def test_mirror_frozen_sizes_and_offsets_match_header():
    text = HEADER.read_text(encoding="utf-8")
    sizes = {int(v): int(n) for v, n in re.findall(r"sizeof\(zone_cfg_v(\d+)_t\)\s*==\s*(\d+)", text)}
    for v, z in cc._ZONE_SIZE_BY_VERSION.items():
        if v == cc.ZONES_CFG_VERSION:
            continue
        assert sizes.get(v) == z, f"zone_cfg_v{v}_t size drifted: header {sizes.get(v)} vs {z}"
    blank = cc._decode_zone_cfg(bytes(ZSZ))
    f1 = struct.unpack("<f", b"\x01\x01\x01\x01")[0]  # every byte non-zero, so offset == first non-zero byte
    probes = {"progress_band_c": f1, "hyst_c": f1, "model_fit_temp_c": f1, "model_fit_ambient_c": f1,
              "coil_power_w": f1, "zone_type": 7, "failsafe_state": 9, "min_on_s": 0x0102, "min_off_s": 0x0304}
    offs = {}
    for name, val in probes.items():
        z = dict(blank)
        z[name] = val
        raw = cc._encode_zone_cfg(z)
        offs[name] = next(i for i, b in enumerate(raw) if b != 0 and i >= 200)
    seen = 0
    for v, name, off in re.findall(r"offsetof\(zone_cfg_v(\d+)_t,\s*(\w+)\)\s*==\s*(\d+)", text):
        if name in offs:
            seen += 1
            assert offs[name] == int(off), f"v{v} {name}: header {off} vs python {offs[name]}"
    assert seen >= 8


def test_mirror_detects_mutated_size():
    # negative test of the mirror: a header with a bumped size must be seen as a mismatch.
    text = HEADER.read_text(encoding="utf-8").replace("sizeof(zone_cfg_v24_t) == 240", "sizeof(zone_cfg_v24_t) == 244")
    sizes = {int(v): int(n) for v, n in re.findall(r"sizeof\(zone_cfg_v(\d+)_t\)\s*==\s*(\d+)", text)}
    assert sizes[24] != cc._ZONE_SIZE_BY_VERSION[24]
