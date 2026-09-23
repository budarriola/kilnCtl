"""Unit tests for kilnctrl.kilnlink_codec's GET_PARAM/PARAM pair.

Byte vectors below are hand-computed against firmware/CommonFW/src/
kilnlink_get_param.c and kilnlink_param.c's own "Offsets" layout (no
test/vectors/*.json exists for these two frames yet -- see this module's own
docstring for why most codecs here are vector-driven and this pair isn't).

Run with the main-tree venv only:
    tools\\PcTools\\.venv\\Scripts\\python.exe -m pytest tools/PcTools/tests/test_kilnlink_codec.py
"""

import pytest

from kilnctrl.kilnlink_codec import decode_param, encode_get_param


def test_encode_get_param_byte_exact():
    # cmd(1)=0x23, param_id=0x0505 as u16 LE -> 05 05
    assert encode_get_param({"param_id": 0x0505}) == bytes([0x23, 0x05, 0x05])


def test_encode_get_param_zero_id():
    assert encode_get_param({"param_id": 0}) == bytes([0x23, 0x00, 0x00])


def test_encode_get_param_max_id():
    assert encode_get_param({"param_id": 0xFFFF}) == bytes([0x23, 0xFF, 0xFF])


def test_encode_get_param_golden_vector_non_palindromic():
    # 0x0505/0x0000/0xFFFF above are byte-palindromic -- LE and BE encode
    # them identically, so those tests alone would still pass a codec that
    # silently used ">BH" (big-endian) instead of "<BH". This is the C
    # golden vector from firmware/CommonFW/test/test_get_param.c's
    # test_vector(): param_id=0x1234 -> {0x23, 0x34, 0x12}, LE-only.
    assert encode_get_param({"param_id": 0x1234}) == bytes([0x23, 0x34, 0x12])


def test_decode_param_not_found():
    # cmd(1)=0x1E, param_id(2)=0x0505 LE, found(1)=0, type(1)=0 -- exactly 5 bytes
    payload = bytes([0x1E, 0x05, 0x05, 0x00, 0x00])
    result = decode_param(payload)
    assert result == {"param_id": 0x0505, "found": 0, "type": 0, "value": None}


def test_decode_param_not_found_golden_vector():
    # firmware/CommonFW/test/test_param.c's test_vector_not_found():
    # {0x1e, 0xff, 0xff, 0x00, 0x00} -- param_id=0xFFFF, found=0.
    payload = bytes([0x1E, 0xFF, 0xFF, 0x00, 0x00])
    result = decode_param(payload)
    assert result == {"param_id": 0xFFFF, "found": 0, "type": 0, "value": None}


def test_decode_param_found_bool():
    # type 0x00 = bool, 1 value byte
    payload = bytes([0x1E, 0x01, 0x00, 0x01, 0x00, 0x01])
    result = decode_param(payload)
    assert result == {"param_id": 1, "found": 1, "type": 0x00, "value": True}


def test_decode_param_bool_out_of_range_value_rejected():
    # A bool value byte other than 0/1 is a malformed reply, not a truthy
    # value -- bool(2) == True would otherwise silently accept it.
    payload = bytes([0x1E, 0x01, 0x00, 0x01, 0x00, 0x02])
    with pytest.raises(ValueError, match="out-of-range bool value"):
        decode_param(payload)


def test_decode_param_found_u8():
    payload = bytes([0x1E, 0x02, 0x00, 0x01, 0x01, 0x2A])
    result = decode_param(payload)
    assert result == {"param_id": 2, "found": 1, "type": 0x01, "value": 0x2A}


def test_decode_param_found_u16():
    # type 0x02 = u16, 2 value bytes LE: 0x1234 -> 34 12
    payload = bytes([0x1E, 0x03, 0x00, 0x01, 0x02, 0x34, 0x12])
    result = decode_param(payload)
    assert result == {"param_id": 3, "found": 1, "type": 0x02, "value": 0x1234}


def test_decode_param_found_f32():
    import struct

    value_bytes = struct.pack("<f", 123.5)
    payload = bytes([0x1E, 0x04, 0x00, 0x01, 0x03]) + value_bytes
    result = decode_param(payload)
    assert result["param_id"] == 4
    assert result["found"] == 1
    assert result["type"] == 0x03
    assert result["value"] == pytest.approx(123.5)


def test_decode_param_too_short():
    with pytest.raises(ValueError, match="too short"):
        decode_param(bytes([0x1E, 0x00, 0x00, 0x00]))


def test_decode_param_wrong_cmd():
    with pytest.raises(ValueError, match="wrong cmd byte"):
        decode_param(bytes([0x1F, 0x00, 0x00, 0x00, 0x00]))


def test_decode_param_bad_found_byte():
    with pytest.raises(ValueError, match="bad found byte"):
        decode_param(bytes([0x1E, 0x00, 0x00, 0x02, 0x00]))


def test_decode_param_not_found_wrong_length():
    with pytest.raises(ValueError, match="'not found' payload must be 5 bytes"):
        decode_param(bytes([0x1E, 0x00, 0x00, 0x00, 0x00, 0xFF]))


def test_decode_param_unknown_type_tag():
    with pytest.raises(ValueError, match="unknown type tag"):
        decode_param(bytes([0x1E, 0x00, 0x00, 0x01, 0x99, 0x00]))


def test_decode_param_length_mismatch_for_type():
    # type 0x02 (u16) implies 7 bytes total, but only 6 given
    with pytest.raises(ValueError, match="length mismatch"):
        decode_param(bytes([0x1E, 0x00, 0x00, 0x01, 0x02, 0x00]))


def test_round_trip_get_param_then_param_found():
    request = encode_get_param({"param_id": 0x0505})
    assert request[0] == 0x23
    reply = bytes([0x1E, 0x05, 0x05, 0x01, 0x01, 0x07])
    result = decode_param(reply)
    assert result == {"param_id": 0x0505, "found": 1, "type": 0x01, "value": 7}
