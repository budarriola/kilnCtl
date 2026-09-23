"""test_config_convert.py -- tests for kilnctrl.config_convert, the
multi-store dispatcher (backup document delegated to kilnctrl.cfg_convert;
the standalone profile_blob and safety_config_blob NVS records implemented
here). See that module's docstring for scope and for what is deliberately
NOT supported yet (kiln_cfg_store's package format, the ESP's raw
zones_cfg_t blob).
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


def test_detect_kind_safety_config_blob():
    fields = cc.safety_config_default_fields()
    blob = cc.encode_safety_config_v3(fields)
    assert cc.detect_kind({"kind": "kilnctl_safety_config_blob", "version": 3, "blob_hex": blob.hex()}) == \
        "safety_config_blob"


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
# safety_config_blob (SaftyFW config_store_record_t) -- golden vectors built
# from firmware's own REC_OFF_* offsets (config_store.c), not from this
# module's own encoder, so a decode bug and an encode bug cannot cancel out.
# ---------------------------------------------------------------------------

_SC_MAGIC_BYTES = struct.pack("<I", 0x4B4C4331)  # CONFIG_STORE_MAGIC


def _fields_approx_equal(a, b):
    """Recursive equality tolerant of float32 round-trip precision loss --
    every numeric field in the wire format is a 32-bit float, so a value
    written as a Python double (e.g. 33.3) and read back never compares
    bit-exact to the original literal."""
    if isinstance(a, float) or isinstance(b, float):
        return a == pytest.approx(b, abs=1e-4)
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_fields_approx_equal(a[k], b[k]) for k in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(_fields_approx_equal(x, y) for x, y in zip(a, b))
    return a == b


def _sc_crc(body: bytes) -> bytes:
    return struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def _build_v3_blob_from_defaults():
    """A literal, hand-assembled v3 record matching config_store_default()'s
    compiled values at every REC_OFF_* offset -- independent of
    encode_safety_config_v3(), so decode is checked against firmware's own
    constants, not against this module's own encoder."""
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 3)  # format_version
    struct.pack_into("<H", buf, 6, 0)  # reserved0
    struct.pack_into("<I", buf, 8, 0)  # seq
    struct.pack_into("<I", buf, 12, 0)  # fields_set
    buf[16] = 0  # tc_source
    buf[17] = 0  # borrowed_zone_index
    buf[18] = 0  # tc_placement_mode
    struct.pack_into("<f", buf, 19, 0.0)  # abs_max_temp_c
    buf[23] = 0x03  # tc_type = K
    buf[24:27] = b"\xff\xff\xff"  # ct_channel_map
    buf[27] = 1  # calibration_missing = true
    struct.pack_into("<f", buf, 28, 100.0)  # firing_margin_c
    struct.pack_into("<f", buf, 32, 75.0)  # overshoot_margin_c
    struct.pack_into("<I", buf, 36, 120)  # overshoot_time_s
    struct.pack_into("<f", buf, 40, 33.3)  # max_rate_c_per_min
    struct.pack_into("<I", buf, 44, 60)  # rate_window_s
    struct.pack_into("<I", buf, 48, 60)  # blind_grace_s
    struct.pack_into("<I", buf, 52, 600)  # frozen_window_s
    struct.pack_into("<f", buf, 56, 200.0)  # tc_disagreement_c
    struct.pack_into("<I", buf, 60, 300)  # tc_disagreement_time_s
    struct.pack_into("<f", buf, 64, 0.0)  # tc_expected_offset_c
    struct.pack_into("<f", buf, 68, 60.0)  # cj_warn_c
    struct.pack_into("<f", buf, 72, 85.0)  # cj_max_c
    struct.pack_into("<I", buf, 76, 60)  # cj_time_s
    struct.pack_into("<I", buf, 80, 10)  # borrowed_stale_s
    struct.pack_into("<I", buf, 84, 60)  # borrowed_stale_trip_s
    buf[88] = 0x03  # borrowed_type_expected
    struct.pack_into("<f", buf, 89, 2.0)  # i_present_a
    struct.pack_into("<H", buf, 93, 0)
    struct.pack_into("<H", buf, 95, 0)
    struct.pack_into("<H", buf, 97, 0)  # zero_counts[3]
    struct.pack_into("<I", buf, 99, 150)  # correlation_window_s
    struct.pack_into("<I", buf, 103, 20)  # stuck_on_time_s
    struct.pack_into("<I", buf, 107, 10)  # trip_verify_s
    for i in range(3):
        struct.pack_into("<f", buf, 111 + i * 4, 0.0)  # k_ct_v_per_a
    for i in range(3):
        struct.pack_into("<f", buf, 123 + i * 4, 0.715)  # gain
    struct.pack_into("<f", buf, 135, 0.0)  # mains_voltage_v
    struct.pack_into("<I", buf, 139, 120)  # power_window_s
    struct.pack_into("<I", buf, 143, 5)  # context_max_age_s
    struct.pack_into("<I", buf, 147, 10)  # link_timeout_s
    struct.pack_into("<I", buf, 151, 120)  # link_dead_hard_s
    struct.pack_into("<I", buf, 155, 200)  # mainfault_debounce_ms
    struct.pack_into("<I", buf, 159, 500)  # telemetry_period_ms
    struct.pack_into("<I", buf, 163, 60)  # startup_grace_s
    struct.pack_into("<I", buf, 167, 50)  # estop_debounce_ms
    struct.pack_into("<I", buf, 171, 1000)  # watchdog_timeout_ms
    struct.pack_into("<I", buf, 175, 10)  # config_check_period_s
    for ch in range(3):
        base = 179 + ch * 9
        buf[base] = 0  # calibrated = false
        struct.pack_into("<f", buf, base + 1, 0.0)
        struct.pack_into("<f", buf, base + 5, 0.0)
    buf[206] = 0x01  # safety_tc_installed marker: installed
    struct.pack_into("<f", buf, 207, 0.0)  # max_expected_power_w
    for i in range(3):
        struct.pack_into("<f", buf, 211 + i * 4, 0.0)  # i_normal_a
    struct.pack_into("<H", buf, 223, 0)  # overcurrent_pct
    struct.pack_into("<I", buf, 225, 0)  # overcurrent_time_s
    buf[229] = 0x01  # ct_installed marker: installed
    buf[230] = 0  # ct_topology = PER_ZONE
    buf[231] = 0  # i_present_a_manual = false
    struct.pack_into("<f", buf, 232, 0.0)  # tc_offset_c
    buf[236] = 0  # estop_active_level = ACTIVE_HIGH
    buf[237] = 0
    buf[238] = 1
    buf[239] = 2  # zone_ct_channel identity map
    buf[240:240 + 264] = b"\x00" * 264  # reserved
    crc = _sc_crc(bytes(buf[:504]))
    buf[504:508] = crc
    return bytes(buf)


def test_decode_safety_config_v3_matches_config_store_default_literal_bytes():
    blob = _build_v3_blob_from_defaults()
    version, fields = cc.decode_safety_config_blob(blob)
    assert version == 3
    assert fields["tc_type"] == 0x03
    assert fields["firing_margin_c"] == 100.0
    assert fields["max_rate_c_per_min"] == pytest.approx(33.3, abs=1e-4)
    assert fields["gain"] == [pytest.approx(0.715)] * 3
    assert fields["safety_tc_installed"] is True
    assert fields["ct_installed"] is True
    assert fields["ct_topology"] == 0
    assert fields["zone_ct_channel"] == [0, 1, 2]
    assert fields["calibration_missing"] is True


def test_encode_safety_config_v3_round_trips_default_fields():
    fields = cc.safety_config_default_fields()
    blob = cc.encode_safety_config_v3(fields)
    assert blob == _build_v3_blob_from_defaults()
    version, decoded = cc.decode_safety_config_blob(blob)
    assert version == 3
    assert _fields_approx_equal(decoded, fields)


def test_decode_safety_config_wrong_length_refuses():
    blob = _build_v3_blob_from_defaults()[:-1]
    with pytest.raises(cc.ConfigConvertError, match="length"):
        cc.decode_safety_config_blob(blob)


def test_decode_safety_config_bad_magic_refuses():
    blob = bytearray(_build_v3_blob_from_defaults())
    blob[0] ^= 0xFF
    with pytest.raises(cc.ConfigConvertError, match="magic"):
        cc.decode_safety_config_blob(bytes(blob))


def test_decode_safety_config_bad_crc_refuses():
    blob = bytearray(_build_v3_blob_from_defaults())
    blob[100] ^= 0xFF  # inside cj_time_s/borrowed_stale_s, well before the CRC tail
    with pytest.raises(cc.ConfigConvertError, match="CRC mismatch"):
        cc.decode_safety_config_blob(bytes(blob))


def test_decode_safety_config_unknown_version_refuses():
    blob = bytearray(_build_v3_blob_from_defaults())
    struct.pack_into("<H", blob, 4, 9)  # bogus format_version
    # recompute CRC over the mutated header so this fails on VERSION, not CRC
    crc = _sc_crc(bytes(blob[:504]))
    blob[504:508] = crc
    with pytest.raises(cc.ConfigConvertError, match="unknown/unsupported"):
        cc.decode_safety_config_blob(bytes(blob))


def test_decode_safety_config_v1_migrates_forward_with_forced_calibration_missing():
    # v1 layout (frozen, config_store.c REC_V1_OFF_*): magic(0) format_version(4)
    # seq(8) tc_type(12) calibration_missing(13) ct_cal(16, 9B x3) crc(248).
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)  # format_version = 1
    struct.pack_into("<I", buf, 8, 42)  # seq
    buf[12] = 0x03  # tc_type = K
    buf[13] = 0  # calibration_missing byte (irrelevant -- v1 migration forces True)
    for ch in range(3):
        base = 16 + ch * 9
        buf[base] = 1  # calibrated
        struct.pack_into("<f", buf, base + 1, 1.5 + ch)  # gain
        struct.pack_into("<f", buf, base + 5, 0.1 * ch)  # offset
    crc = _sc_crc(bytes(buf[:248]))
    buf[248:252] = crc
    version, fields = cc.decode_safety_config_blob(bytes(buf))
    assert version == 1
    assert fields["seq"] == 42
    assert fields["tc_type"] == 0x03
    assert fields["calibration_missing"] is True  # forced, regardless of the source byte
    assert fields["ct_cal"][0]["calibrated"] is True
    assert fields["ct_cal"][0]["gain"] == pytest.approx(1.5)
    assert fields["format_version"] == 3
    assert fields["zone_ct_channel"] == [0, 1, 2]  # config_store_default()'s identity map


def test_decode_safety_config_v1_bad_crc_refuses():
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)
    struct.pack_into("<I", buf, 8, 1)
    buf[12] = 0x03
    buf[248:252] = struct.pack("<I", 0xDEADBEEF)  # deliberately wrong CRC
    with pytest.raises(cc.ConfigConvertError, match="CRC mismatch"):
        cc.decode_safety_config_blob(bytes(buf))


def test_decode_safety_config_v2_migrates_zone_ct_channel_from_topology():
    # v2 layout (frozen, config_store.c REC_V2_OFF_*): same as v3 below
    # offset 14, fields_set is a u16 at 12, tc_source starts at 14 (v3's
    # offset - 2), reserved starts at 235, crc at 504. Build it by taking a
    # known-good v3 default blob and shifting the field block DOWN by 2
    # bytes -- the exact inverse of decode_safety_config_blob()'s v2->v3
    # migration -- so this test exercises the real shift math both ways.
    v3 = bytearray(_build_v3_blob_from_defaults())
    v2 = bytearray(b"\xff" * 512)
    v2[0:12] = v3[0:12]  # magic, format_version, reserved0, seq
    struct.pack_into("<H", v2, 4, 2)  # format_version = 2
    struct.pack_into("<H", v2, 12, 0)  # fields_set (u16 at v2) -- nothing set
    v2[14:14 + (235 - 14)] = v3[16:16 + (235 - 14)]  # tc_source..just-before-reserved, shifted -2
    v2[235:235 + 264] = v3[240:240 + 264]  # reserved tail (same length both versions)
    crc = _sc_crc(bytes(v2[:504]))
    v2[504:508] = crc
    version, fields = cc.decode_safety_config_blob(bytes(v2))
    assert version == 2
    assert fields["format_version"] == 3
    assert fields["ct_topology"] == 0  # PER_ZONE (byte 230 in the shifted block was 0)
    assert fields["zone_ct_channel"] == [0, 1, 2]  # derived from PER_ZONE, not read from the wire
    assert fields["calibration_missing"] is True  # carried through from the v3-shaped bytes, not forced


def test_convert_safety_config_blob_v1_to_v3():
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)
    struct.pack_into("<I", buf, 8, 7)
    buf[12] = 0x03
    buf[13] = 1
    for ch in range(3):
        base = 16 + ch * 9
        buf[base] = 0
    crc = _sc_crc(bytes(buf[:248]))
    buf[248:252] = crc
    out, report = cc.convert_safety_config_blob(bytes(buf), 3)
    version, fields = cc.decode_safety_config_blob(out)
    assert version == 3
    assert fields["seq"] == 7
    assert report.source_version == 1
    assert report.target_version == 3
    assert any(o.action == "defaulted" for o in report.outcomes)


def test_convert_safety_config_blob_refuses_downgrade():
    fields = cc.safety_config_default_fields()
    blob = cc.encode_safety_config_v3(fields)
    with pytest.raises(cc.ConfigConvertError, match="not supported for ENCODING"):
        cc.convert_safety_config_blob(blob, 2)


def test_convert_document_safety_config_blob_wrapper():
    fields = cc.safety_config_default_fields()
    blob = cc.encode_safety_config_v3(fields)
    doc = {"kind": "kilnctl_safety_config_blob", "version": 3, "blob_hex": blob.hex()}
    out_doc, report = cc.convert_document(doc, 3)
    assert out_doc["kind"] == "kilnctl_safety_config_blob"
    assert out_doc["version"] == 3
    _, decoded = cc.decode_safety_config_blob(bytes.fromhex(out_doc["blob_hex"]))
    assert _fields_approx_equal(decoded, fields)


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
