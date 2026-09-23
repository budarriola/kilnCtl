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


def _build_v3_golden_nontrivial():
    """A second hand-built v3 record, distinct-valued at (almost) every
    REC_OFF_* field -- unlike _build_v3_blob_from_defaults() this deliberately
    avoids zeros/equal-across-fields/palindromic values so a mutation that
    swaps two field offsets, narrows a mask, or drops a fold/clamp changes a
    concrete assertion rather than silently agreeing with a default or a
    zero. Returns (raw_blob, normalized_blob, expected_fields):
      - raw_blob: what a real (corrupt-ish, out-of-range zone_ct_channel)
        record on flash might look like.
      - normalized_blob: raw_blob with the fields decode_safety_config_blob()
        is required to CORRECT (the FIELDS_SET clear-mask and the derived
        ZONE_CT_CHANNEL) patched to their expected post-decode values, and
        the CRC recomputed over that corrected body -- this is what
        encode_safety_config_v3(expected_fields) must reproduce byte-for-byte,
        since decode is a normalizing operation and its output does not
        round-trip back to raw_blob bit-for-bit by design.
      - expected_fields: the exact dict decode_safety_config_blob() must
        return for raw_blob.
    """
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 3)
    struct.pack_into("<H", buf, 6, 0)
    struct.pack_into("<I", buf, 8, 0x01020304)  # seq
    struct.pack_into("<I", buf, 12, 0x0008C001)  # fields_set (bit19 in the ZONE_CT_CHANNEL mask, plus 14/15/0)
    buf[16] = 7  # tc_source
    buf[17] = 5  # borrowed_zone_index
    buf[18] = 2  # tc_placement_mode
    struct.pack_into("<f", buf, 19, 999.5)  # abs_max_temp_c
    buf[23] = 0x0A  # tc_type -- out of range (> CONFIG_STORE_TC_TYPE_MAX_REAL=7), must clamp to default 3
    buf[24:27] = bytes([11, 22, 33])  # ct_channel_map (no clamp defined for this field)
    buf[27] = 1  # calibration_missing
    struct.pack_into("<f", buf, 28, 123.25)  # firing_margin_c
    struct.pack_into("<f", buf, 32, 45.75)  # overshoot_margin_c
    struct.pack_into("<I", buf, 36, 777)  # overshoot_time_s
    struct.pack_into("<f", buf, 40, 12.125)  # max_rate_c_per_min
    struct.pack_into("<I", buf, 44, 61)  # rate_window_s
    struct.pack_into("<I", buf, 48, 62)  # blind_grace_s
    struct.pack_into("<I", buf, 52, 603)  # frozen_window_s
    struct.pack_into("<f", buf, 56, 201.5)  # tc_disagreement_c
    struct.pack_into("<I", buf, 60, 301)  # tc_disagreement_time_s
    struct.pack_into("<f", buf, 64, 1.5)  # tc_expected_offset_c
    struct.pack_into("<f", buf, 68, 61.5)  # cj_warn_c
    struct.pack_into("<f", buf, 72, 86.5)  # cj_max_c
    struct.pack_into("<I", buf, 76, 63)  # cj_time_s
    struct.pack_into("<I", buf, 80, 11)  # borrowed_stale_s
    struct.pack_into("<I", buf, 84, 64)  # borrowed_stale_trip_s
    buf[88] = 4  # borrowed_type_expected
    struct.pack_into("<f", buf, 89, 2.5)  # i_present_a
    struct.pack_into("<H", buf, 93, 100)
    struct.pack_into("<H", buf, 95, 200)
    struct.pack_into("<H", buf, 97, 300)  # zero_counts[3]
    struct.pack_into("<I", buf, 99, 151)  # correlation_window_s
    struct.pack_into("<I", buf, 103, 21)  # stuck_on_time_s
    struct.pack_into("<I", buf, 107, 12)  # trip_verify_s
    for i, v in enumerate((0.1, 0.2, 0.3)):
        struct.pack_into("<f", buf, 111 + i * 4, v)  # k_ct_v_per_a
    for i, v in enumerate((1.1, 1.2, 1.3)):
        struct.pack_into("<f", buf, 123 + i * 4, v)  # gain
    struct.pack_into("<f", buf, 135, 240.5)  # mains_voltage_v
    struct.pack_into("<I", buf, 139, 121)  # power_window_s
    struct.pack_into("<I", buf, 143, 6)  # context_max_age_s
    struct.pack_into("<I", buf, 147, 11)  # link_timeout_s
    struct.pack_into("<I", buf, 151, 121)  # link_dead_hard_s
    struct.pack_into("<I", buf, 155, 201)  # mainfault_debounce_ms
    struct.pack_into("<I", buf, 159, 501)  # telemetry_period_ms
    struct.pack_into("<I", buf, 163, 61)  # startup_grace_s
    struct.pack_into("<I", buf, 167, 51)  # estop_debounce_ms
    struct.pack_into("<I", buf, 171, 1001)  # watchdog_timeout_ms
    struct.pack_into("<I", buf, 175, 11)  # config_check_period_s
    ct_cal_src = [(1, 1.5, 0.5), (0, 2.5, 1.5), (1, 3.5, 2.5)]
    for ch, (calibrated, gain, offset) in enumerate(ct_cal_src):
        base = 179 + ch * 9
        buf[base] = calibrated
        struct.pack_into("<f", buf, base + 1, gain)
        struct.pack_into("<f", buf, base + 5, offset)
    buf[206] = 0xA5  # safety_tc_installed marker: NOT installed
    struct.pack_into("<f", buf, 207, 1500.5)  # max_expected_power_w -- distinct from every i_normal_a below
    for i, v in enumerate((3.5, 4.5, 5.5)):
        struct.pack_into("<f", buf, 211 + i * 4, v)  # i_normal_a
    struct.pack_into("<H", buf, 223, 0xFFFF)  # overcurrent_pct -- must fold to 0
    struct.pack_into("<I", buf, 225, 0xFFFFFFFF)  # overcurrent_time_s -- must fold to 0
    buf[229] = 0xA5  # ct_installed marker: NOT installed
    buf[230] = 1  # ct_topology = SUMMED
    buf[231] = 1  # i_present_a_manual = true
    struct.pack_into("<f", buf, 232, 9.5)  # tc_offset_c
    buf[236] = 1  # estop_active_level = ACTIVE_LOW
    buf[237] = 0
    buf[238] = 1
    buf[239] = 3  # zone_ct_channel -- byte 3 is out of range ( > 2), must trigger the derive fallback
    reserved = bytes(i % 256 for i in range(264))  # ascending, not palindromic, not all-equal
    buf[240:240 + 264] = reserved
    crc = _sc_crc(bytes(buf[:504]))
    buf[504:508] = crc
    raw_blob = bytes(buf)

    # normalized_blob: same as raw_blob except every field decode_safety_config_blob()
    # is required to CORRECT rather than pass through verbatim is patched to its
    # expected post-decode value: FIELDS_SET has bits 16-19 cleared, ZONE_CT_CHANNEL
    # is the SUMMED-topology derivation, TC_TYPE is clamped, and OVERCURRENT_PCT/
    # OVERCURRENT_TIME_S are folded to 0 -- exactly what
    # encode_safety_config_v3(expected_fields) must reproduce byte-for-byte.
    norm = bytearray(raw_blob)
    # 0x0008C001 & ~0x000F0000: only bit19 (part of 0x00080000) falls inside the
    # mask -- bits 0/14/15 are untouched -- so this clears the "8" nibble only.
    normalized_fields_set = 0x0008C001 & ~0x000F0000  # == 0x0000C001
    struct.pack_into("<I", norm, 12, normalized_fields_set)
    norm[23] = 0x03  # tc_type clamped from the out-of-range 0x0A
    struct.pack_into("<H", norm, 223, 0)  # overcurrent_pct folded from 0xFFFF
    struct.pack_into("<I", norm, 225, 0)  # overcurrent_time_s folded from 0xFFFFFFFF
    norm[237:240] = bytes([2, 2, 2])
    norm_crc = _sc_crc(bytes(norm[:504]))
    norm[504:508] = norm_crc
    normalized_blob = bytes(norm)

    expected_fields = {
        "format_version": 3, "seq": 0x01020304, "fields_set": normalized_fields_set,
        "tc_source": 7, "borrowed_zone_index": 5, "tc_placement_mode": 2, "abs_max_temp_c": 999.5,
        "tc_type": 0x03,  # clamped from the out-of-range 0x0A
        "ct_channel_map": [11, 22, 33], "calibration_missing": True,
        "firing_margin_c": 123.25, "overshoot_margin_c": 45.75, "overshoot_time_s": 777,
        "max_rate_c_per_min": 12.125, "rate_window_s": 61, "blind_grace_s": 62, "frozen_window_s": 603,
        "tc_disagreement_c": 201.5, "tc_disagreement_time_s": 301, "tc_expected_offset_c": 1.5,
        "cj_warn_c": 61.5, "cj_max_c": 86.5, "cj_time_s": 63, "borrowed_stale_s": 11,
        "borrowed_stale_trip_s": 64, "borrowed_type_expected": 4, "i_present_a": 2.5,
        "zero_counts": [100, 200, 300], "correlation_window_s": 151, "stuck_on_time_s": 21,
        "trip_verify_s": 12, "k_ct_v_per_a": [0.1, 0.2, 0.3], "gain": [1.1, 1.2, 1.3],
        "mains_voltage_v": 240.5, "power_window_s": 121, "context_max_age_s": 6,
        "link_timeout_s": 11, "link_dead_hard_s": 121, "mainfault_debounce_ms": 201,
        "telemetry_period_ms": 501, "startup_grace_s": 61, "estop_debounce_ms": 51,
        "watchdog_timeout_ms": 1001, "config_check_period_s": 11,
        "ct_cal": [
            {"calibrated": True, "gain": 1.5, "offset": 0.5},
            {"calibrated": False, "gain": 2.5, "offset": 1.5},
            {"calibrated": True, "gain": 3.5, "offset": 2.5},
        ],
        "safety_tc_installed": False,  # 0xA5 marker
        "max_expected_power_w": 1500.5,
        "i_normal_a": [3.5, 4.5, 5.5],
        "overcurrent_pct": 0, "overcurrent_time_s": 0,  # folded from 0xFFFF/0xFFFFFFFF
        "ct_installed": False,  # 0xA5 marker
        "ct_topology": 1,  # SUMMED
        "i_present_a_manual": True,
        "tc_offset_c": 9.5,
        "estop_active_level": 1,  # ACTIVE_LOW
        "zone_ct_channel": [2, 2, 2],  # derived, SUMMED topology -- NOT [0,1,2]
        "reserved_hex": reserved.hex(),
    }
    return raw_blob, normalized_blob, expected_fields


def test_decode_safety_config_v3_golden_nontrivial_every_field():
    raw_blob, normalized_blob, expected_fields = _build_v3_golden_nontrivial()
    version, decoded = cc.decode_safety_config_blob(raw_blob)
    assert version == 3
    assert _fields_approx_equal(decoded, expected_fields)
    # encode(decode(x)) reproduces the NORMALIZED blob byte-for-byte -- not
    # raw_blob, since decode is required to correct the out-of-range
    # zone_ct_channel and its fields_set bits, not preserve them.
    assert cc.encode_safety_config_v3(decoded) == normalized_blob


def test_decode_safety_config_v3_golden_nontrivial_fields_set_mask_is_exact():
    # Isolates the clear-mask assertion: bit19 (part of 0x000F0000) must be
    # cleared, but bit0/14/15 (outside the mask) must survive untouched. A
    # mutation that narrows or drops this mask (e.g. only clearing bit16)
    # changes this exact value.
    raw_blob, _, expected_fields = _build_v3_golden_nontrivial()
    _, decoded = cc.decode_safety_config_blob(raw_blob)
    assert decoded["fields_set"] == 0x0000C001
    assert expected_fields["fields_set"] == 0x0000C001


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


def test_decode_safety_config_v1_clamps_out_of_range_tc_type():
    # tc_type byte 0x0A exceeds _SC_TC_TYPE_MAX_REAL (0x07) -- must clamp to
    # the default (K, 0x03), not pass an invalid enum value through.
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)  # format_version = 1
    struct.pack_into("<I", buf, 8, 1)  # seq
    buf[12] = 0x0A  # tc_type, out of range
    buf[13] = 0
    for ch in range(3):
        base = 16 + ch * 9
        buf[base] = 0
        struct.pack_into("<f", buf, base + 1, 0.0)
        struct.pack_into("<f", buf, base + 5, 0.0)
    crc = _sc_crc(bytes(buf[:248]))
    buf[248:252] = crc
    version, fields = cc.decode_safety_config_blob(bytes(buf))
    assert version == 1
    assert fields["tc_type"] == 3


def test_decode_safety_config_v1_bad_crc_refuses():
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)
    struct.pack_into("<I", buf, 8, 1)
    buf[12] = 0x03
    buf[248:252] = struct.pack("<I", 0xDEADBEEF)  # deliberately wrong CRC
    with pytest.raises(cc.ConfigConvertError, match="CRC mismatch"):
        cc.decode_safety_config_blob(bytes(buf))


def _build_v2_blob(fields_set_u16, ct_topology_byte, last_shifted_byte, first_reserved_byte):
    """v2 layout (frozen, config_store.c REC_V2_OFF_*): magic/format_version/
    reserved0/seq occupy the same 0-11 bytes as v3; fields_set is a u16 at
    12 (not u32); the shifted field block runs v2 offset 14 (tc_source) to
    234 (estop_active_level, v3's offset - 2) inclusive; reserved starts at
    235 (269 B declared, only the first 264 are ever copied forward); crc at
    504. v2 has no zone_ct_channel field at all -- it is always derived on
    migration, never read off a v2 record."""
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 2)  # format_version = 2
    struct.pack_into("<H", buf, 6, 0)  # reserved0
    struct.pack_into("<I", buf, 8, 0x0A0B0C0D)  # seq
    struct.pack_into("<H", buf, 12, fields_set_u16)  # fields_set (u16 at v2)
    buf[14] = 0  # tc_source (v2 offset 14 == v3 offset 16)
    buf[228] = ct_topology_byte  # ct_topology (v3 offset 230 - 2)
    buf[234] = last_shifted_byte  # estop_active_level (v3 offset 236 - 2) -- LAST shifted byte
    buf[235] = first_reserved_byte  # FIRST reserved byte (copied 1:1, not shifted)
    buf[236:235 + 264] = b"\x00" * (235 + 264 - 236)
    crc = _sc_crc(bytes(buf[:504]))
    buf[504:508] = crc
    return bytes(buf)


def test_decode_safety_config_v2_migrates_zone_ct_channel_from_topology():
    blob = _build_v2_blob(fields_set_u16=0x8001, ct_topology_byte=1,
                           last_shifted_byte=1, first_reserved_byte=0xAB)
    version, fields = cc.decode_safety_config_blob(blob)
    assert version == 2
    assert fields["format_version"] == 3
    assert fields["seq"] == 0x0A0B0C0D
    # zero-EXTENSION, not a shift: 0x8001 (u16) -> 0x00008001 (u32), never
    # 0x80010000 or any other bit-shuffled variant.
    assert fields["fields_set"] == 0x00008001
    assert fields["ct_topology"] == 1  # SUMMED (byte 228 == v3 offset 230 - 2)
    assert fields["zone_ct_channel"] == [2, 2, 2]  # derived from SUMMED, not [0,1,2]
    assert fields["estop_active_level"] == 1  # last byte the shift must carry (v2 234 -> v3 236)
    assert fields["reserved_hex"].startswith("ab")  # first reserved byte, copied 1:1 (v2 235 -> v3 240)
    assert fields["calibration_missing"] is True  # carried through from the v3-shaped bytes, not forced


def test_decode_safety_config_v2_bad_crc_refuses():
    blob = bytearray(_build_v2_blob(fields_set_u16=0, ct_topology_byte=0,
                                     last_shifted_byte=0, first_reserved_byte=0))
    blob[300] ^= 0xFF  # inside the reserved tail, well before the CRC
    with pytest.raises(cc.ConfigConvertError, match="CRC mismatch"):
        cc.decode_safety_config_blob(bytes(blob))


def test_decode_safety_config_v2_wrong_magic_refuses():
    blob = bytearray(_build_v2_blob(fields_set_u16=0, ct_topology_byte=0,
                                     last_shifted_byte=0, first_reserved_byte=0))
    blob[0] ^= 0xFF
    with pytest.raises(cc.ConfigConvertError, match="magic"):
        cc.decode_safety_config_blob(bytes(blob))


def test_decode_safety_config_v2_truncated_refuses():
    blob = _build_v2_blob(fields_set_u16=0, ct_topology_byte=0,
                           last_shifted_byte=0, first_reserved_byte=0)[:-1]
    with pytest.raises(cc.ConfigConvertError, match="length"):
        cc.decode_safety_config_blob(blob)


def test_decode_safety_config_v1_wrong_magic_refuses():
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)
    struct.pack_into("<I", buf, 8, 1)
    buf[12] = 0x03
    crc = _sc_crc(bytes(buf[:248]))
    buf[248:252] = crc
    buf[0] ^= 0xFF
    with pytest.raises(cc.ConfigConvertError, match="magic"):
        cc.decode_safety_config_blob(bytes(buf))


def test_decode_safety_config_v1_truncated_refuses():
    buf = bytearray(b"\xff" * 512)
    buf[0:4] = _SC_MAGIC_BYTES
    struct.pack_into("<H", buf, 4, 1)
    struct.pack_into("<I", buf, 8, 1)
    buf[12] = 0x03
    crc = _sc_crc(bytes(buf[:248]))
    buf[248:252] = crc
    with pytest.raises(cc.ConfigConvertError, match="length"):
        cc.decode_safety_config_blob(bytes(buf)[:-1])


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


def test_encode_safety_config_v3_rejects_short_ct_channel_map():
    fields = cc.safety_config_default_fields()
    fields["ct_channel_map"] = [0, 1]
    with pytest.raises(ValueError, match="ct_channel_map"):
        cc.encode_safety_config_v3(fields)


def test_encode_safety_config_v3_rejects_short_zone_ct_channel():
    fields = cc.safety_config_default_fields()
    fields["zone_ct_channel"] = [0, 1]
    with pytest.raises(ValueError, match="zone_ct_channel"):
        cc.encode_safety_config_v3(fields)


def test_encode_safety_config_v3_rejects_wrong_length_reserved_hex():
    fields = cc.safety_config_default_fields()
    fields["reserved_hex"] = "ab"
    with pytest.raises(ValueError, match="reserved_hex"):
        cc.encode_safety_config_v3(fields)


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
