"""test_config_convert.py -- tests for kilnctrl.config_convert, the
multi-store dispatcher (backup document delegated to kilnctrl.cfg_convert;
the standalone profile_blob NVS record implemented here). See that module's
docstring for scope and for what is deliberately NOT supported yet
(kiln_cfg_store's package format, SaftyFW's raw config_store_record_t).
"""
import json
import struct
import zlib

import pytest

from kilnctrl import cfg_convert as backup_cfg_convert
from kilnctrl import config_convert as cc


def _pack_v1(name="Cone06", zone_mask=1, segments=None):
    segments = segments or [{"target_c": 100.0, "ramp_c_per_hr": 60.0, "dwell_min": 10}]
    name_bytes = name.encode("ascii")[:15]
    name_bytes = name_bytes + b"\x00" * (16 - len(name_bytes))
    body = struct.pack("<B3x", 1) + struct.pack("<16sBB2x", name_bytes, zone_mask, len(segments))
    for i in range(cc.PROFILE_MAX_SEGMENTS):
        if i < len(segments):
            s = segments[i]
            body += struct.pack("<ffI", s["target_c"], s["ramp_c_per_hr"], s["dwell_min"])
        else:
            body += struct.pack("<ffI", 0.0, 0.0, 0)
    assert len(body) == 168
    return body


# ---------------------------------------------------------------------------
# detect_kind
# ---------------------------------------------------------------------------


def test_detect_kind_backup():
    assert cc.detect_kind({"kind": "kilnctl_backup", "version": 4}) == "backup"


def test_detect_kind_profile_blob():
    assert cc.detect_kind({"kind": "kilnctl_profile_blob", "version": 1, "blob_hex": "00"}) == "profile_blob"


def test_detect_kind_unknown_refuses():
    with pytest.raises(cc.ConfigConvertError):
        cc.detect_kind({"kind": "something_else"})


def test_detect_kind_kiln_cfg_package_names_the_gap():
    with pytest.raises(cc.ConfigConvertError, match="kiln_package.h"):
        cc.detect_kind({"kind": "kilnctl_kiln_cfg_package"})


def test_detect_kind_safety_config_store_names_the_gap():
    with pytest.raises(cc.ConfigConvertError, match="config_store.c"):
        cc.detect_kind({"kind": "safety_config_store"})


def test_detect_kind_zones_blob_names_the_gap():
    with pytest.raises(cc.ConfigConvertError, match="zones_config_json.h"):
        cc.detect_kind({"kind": "kilnctl_zones_blob"})


def test_detect_kind_not_a_dict():
    with pytest.raises(cc.ConfigConvertError):
        cc.detect_kind(["not", "a", "dict"])


# ---------------------------------------------------------------------------
# profile_blob: decode
# ---------------------------------------------------------------------------


def test_decode_v1_profile_blob():
    blob = _pack_v1()
    version, profile = cc.decode_profile_blob(blob)
    assert version == 1
    assert profile["name"] == "Cone06"
    assert profile["zone_mask"] == 1
    assert len(profile["segments"]) == 1
    assert profile["segments"][0]["target_c"] == 100.0
    assert profile["on_off_rules"] == []


def test_decode_unknown_version_refuses():
    with pytest.raises(cc.ConfigConvertError, match="unknown/unsupported"):
        cc.decode_profile_blob(bytes([9]) + b"\x00" * 20)


def test_decode_wrong_length_refuses():
    blob = _pack_v1()[:-1]  # one byte short of v1's expected length
    with pytest.raises(cc.ConfigConvertError, match="does not match"):
        cc.decode_profile_blob(blob)


def test_decode_empty_refuses():
    with pytest.raises(cc.ConfigConvertError):
        cc.decode_profile_blob(b"")


# ---------------------------------------------------------------------------
# profile_blob: forward conversion (v1 -> v4) mirrors firmware defaults
# ---------------------------------------------------------------------------


def test_forward_v1_to_v4_defaults_relay_io_and_rules():
    blob = _pack_v1()
    out, report = cc.convert_profile_blob(blob, 4)
    version, profile = cc.decode_profile_blob(out)
    assert version == 4
    assert profile["name"] == "Cone06"
    seg = profile["segments"][0]
    assert seg["seg_kind"] == 0  # PROFILE_SEG_KIND_ZONE_RAMP
    assert seg["io_leave_on_at_end"] == 0
    assert profile["on_off_rules"] == []
    assert report.lossy is False  # forward is never lossy: nothing is dropped, only defaulted
    actions = {o.action for o in report.outcomes}
    assert "defaulted" in actions


def test_forward_v1_to_v2_no_shape_change_besides_crc():
    blob = _pack_v1()
    out, report = cc.convert_profile_blob(blob, 2)
    version, profile = cc.decode_profile_blob(out)
    assert version == 2
    assert profile["name"] == "Cone06"
    assert len(out) == cc._EXPECTED_LEN[2]


def test_crc_covers_body_plus_zeroed_crc_field_like_compute_profile_crc():
    """Pins the CRC range to match firmware's compute_profile_crc()
    (profiles_http.c:487-492): the CRC is computed over the WHOLE persisted
    struct with the crc32 field zeroed -- body bytes plus 4 zero bytes, not
    body bytes alone."""
    v1_blob = _pack_v1()
    v2_blob = cc._encode_v1_or_v2(cc.decode_profile_blob(v1_blob)[1], 2)
    body = v2_blob[:-4]
    stored_crc = int.from_bytes(v2_blob[-4:], "little")
    assert stored_crc == zlib.crc32(body + b"\x00\x00\x00\x00") & 0xFFFFFFFF


def test_decode_profile_blob_rejects_flipped_byte_crc_mismatch():
    v1_blob = _pack_v1()
    v2_blob = cc._encode_v1_or_v2(cc.decode_profile_blob(v1_blob)[1], 2)
    corrupted = bytearray(v2_blob)
    corrupted[10] ^= 0xFF  # flip a byte inside the name field (bytes 4-19), well before the CRC tail
    with pytest.raises(cc.ConfigConvertError, match="CRC mismatch"):
        cc.decode_profile_blob(bytes(corrupted))


def test_decode_profile_blob_v1_has_no_crc_check():
    # v1 predates the crc32 tail entirely -- decoding must not attempt one.
    blob = _pack_v1()
    version, profile = cc.decode_profile_blob(blob)
    assert version == 1
    assert profile["name"] == "Cone06"


def test_decode_profile_blob_segment_count_overflow_refuses():
    blob = bytearray(_pack_v1())
    # _HEADER_FMT layout: version(1)+pad(3) + name(16) + zone_mask(1) + segment_count(1) -> byte 21.
    blob[21] = cc.PROFILE_MAX_SEGMENTS + 1
    with pytest.raises(cc.ConfigConvertError, match="segment_count"):
        cc.decode_profile_blob(bytes(blob))


def test_forward_v2_to_v3_adds_relay_io_fields_defaulted():
    v1_blob = _pack_v1()
    v2_blob, _ = cc.convert_profile_blob(v1_blob, 2)
    v3_blob, report = cc.convert_profile_blob(v2_blob, 3)
    version, profile = cc.decode_profile_blob(v3_blob)
    assert version == 3
    assert profile["segments"][0]["seg_kind"] == 0
    assert any(o.action == "defaulted" for o in report.outcomes)


# ---------------------------------------------------------------------------
# profile_blob: same-version and round trips
# ---------------------------------------------------------------------------


def test_same_version_is_lossless_noop():
    blob = _pack_v1()
    out, report = cc.convert_profile_blob(blob, 1)
    assert out == blob
    assert report.lossy is False
    assert report.source_version == report.target_version == 1


def test_round_trip_v1_up_to_v4_and_back_loses_nothing_forward_compatible():
    v1 = _pack_v1(name="TestRT", zone_mask=5, segments=[
        {"target_c": 950.0, "ramp_c_per_hr": 120.0, "dwell_min": 30},
        {"target_c": 1000.0, "ramp_c_per_hr": 0.0, "dwell_min": 15},
    ])
    v4, up_report = cc.convert_profile_blob(v1, 4)
    back_v1, down_report = cc.convert_profile_blob(v4, 1)
    _, orig = cc.decode_profile_blob(v1)
    _, roundtripped = cc.decode_profile_blob(back_v1)
    assert orig["name"] == roundtripped["name"]
    assert orig["zone_mask"] == roundtripped["zone_mask"]
    assert orig["segments"] == roundtripped["segments"]
    assert up_report.lossy is False
    assert down_report.lossy is False  # nothing v1-expressible was dropped


def test_backward_conversion_drops_relay_io_segment_and_reports_it():
    v4_profile = {
        "name": "IoTest", "zone_mask": 1,
        "segments": [{"target_c": 0.0, "ramp_c_per_hr": 0.0, "dwell_min": 5, "seg_kind": 1,
                      "io_target": 1, "io_state": 1, "io_blocking": 1, "io_leave_on_at_end": 0}],
        "on_off_rules": [],
    }
    v4_blob = cc._encode_v4(v4_profile)
    out, report = cc.convert_profile_blob(v4_blob, 1)
    assert report.lossy is True
    dropped = [o for o in report.outcomes if o.action == "dropped"]
    assert dropped
    assert any("relay/IO" in o.detail for o in dropped)
    _, decoded = cc.decode_profile_blob(out)
    assert decoded["segments"][0]["seg_kind"] == 0  # silently coerced to ZONE_RAMP at v1, not preserved


def test_backward_conversion_drops_on_off_rules_and_reports_it():
    v4_profile = {
        "name": "RuleTest", "zone_mask": 1,
        "segments": [{"target_c": 100.0, "ramp_c_per_hr": 10.0, "dwell_min": 5, "seg_kind": 0,
                      "io_target": 0, "io_state": 0, "io_blocking": 0, "io_leave_on_at_end": 0}],
        "on_off_rules": [{**cc._DEFAULT_RULE, "enable": 1, "zone_index": 2}],
    }
    v4_blob = cc._encode_v4(v4_profile)
    out, report = cc.convert_profile_blob(v4_blob, 3)
    assert report.lossy is True
    assert any("on/off-rule" in o.detail for o in report.outcomes if o.action == "dropped")
    _, decoded = cc.decode_profile_blob(out)
    assert decoded["on_off_rules"] == []


def test_unknown_target_version_refuses():
    blob = _pack_v1()
    with pytest.raises(cc.ConfigConvertError, match="not a known PROFILE_VERSION"):
        cc.convert_profile_blob(blob, 99)


# ---------------------------------------------------------------------------
# convert_document dispatch (JSON wrapper)
# ---------------------------------------------------------------------------


def test_convert_document_profile_blob_wrapper():
    blob = _pack_v1()
    doc = {"kind": "kilnctl_profile_blob", "version": 1, "blob_hex": blob.hex()}
    out_doc, report = cc.convert_document(doc, 4)
    assert out_doc["kind"] == "kilnctl_profile_blob"
    assert out_doc["version"] == 4
    _, profile = cc.decode_profile_blob(bytes.fromhex(out_doc["blob_hex"]))
    assert profile["name"] == "Cone06"


def test_convert_document_profile_blob_bad_hex_refuses():
    with pytest.raises(cc.ConfigConvertError, match="blob_hex"):
        cc.convert_document({"kind": "kilnctl_profile_blob", "version": 1, "blob_hex": "not hex"}, 1)


def test_convert_document_backup_delegates_to_cfg_convert():
    doc = {
        "kind": "kilnctl_backup", "version": 4, "profiles": [],
        "zones": [{"index": 0, "pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}],
    }
    out_doc, report = cc.convert_document(doc, 4)
    assert out_doc["kind"] == "kilnctl_backup"
    assert out_doc["version"] == 4
    assert report.store == "backup"


def test_convert_document_backup_rejects_credentials_same_as_cfg_convert():
    doc = {"kind": "kilnctl_backup", "version": 4, "zones": [], "profiles": [], "kiln_auth": {"password": "x"}}
    with pytest.raises(backup_cfg_convert.CfgConvertError):
        cc.convert_document(doc, 4)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def test_cli_round_trip(tmp_path, capsys):
    blob = _pack_v1()
    in_path = tmp_path / "in.json"
    in_path.write_text(json.dumps({"kind": "kilnctl_profile_blob", "version": 1, "blob_hex": blob.hex()}))
    out_path = tmp_path / "out.json"
    rc = cc.main([str(in_path), "--to-version", "4", "-o", str(out_path), "--quiet"])
    assert rc == 0
    out_doc = json.loads(out_path.read_text())
    assert out_doc["version"] == 4
    _, profile = cc.decode_profile_blob(bytes.fromhex(out_doc["blob_hex"]))
    assert profile["name"] == "Cone06"


def test_cli_bad_input_reports_error(tmp_path, capsys):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"kind": "nope"}))
    rc = cc.main([str(bad), "--to-version", "1", "--quiet"])
    assert rc == 1
    assert "error" in capsys.readouterr().err
