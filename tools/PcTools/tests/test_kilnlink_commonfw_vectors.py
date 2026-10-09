"""Round-trip the pc_tools kilnlink codecs against firmware/CommonFW's shared
test/vectors/*.json -- tools/PcTools/TODO.md's "Python codec checked against
firmware/CommonFW/test/vectors/" item.

Two vector-file conventions exist there (see the individual files' own
`_comment`/`note` fields):

  * "flat" files (announce/context/fw_version/power/status): a single vector
    dict carries the field values directly at the top level, plus
    `payload_hex`.
  * "fields" files (ceiling/clear_trip/diag/get_fw_version/set_clock/trip):
    each vector nests its field values under a `fields` dict, plus
    `bytes_hex`.

kilnctrl.kilnlink_codec is encode-only and deliberately covers only the
payload codecs that have both a host test and a vectors file in CommonFW
(see its module docstring) -- announce/context/status/diag/trip/power/
ceiling/clear_trip/get_fw_version/set_clock. Those ten are checked here by
re-encoding each vector's fields and comparing to the vector's expected
bytes.

Three vector files are intentionally not exercised through this codec
module, each for a documented reason (not an oversight):

  * set_config_vectors.json -- SAFETY_CMD_SET_CONFIG already has a byte-exact
    check against kilnctrl.devices.safety_set_config() in
    test_safety_set_config.py; kilnlink_codec.py has no encode_set_config.
  * fw_version_vectors.json -- this is the Pico->ESP *response* frame
    (protocol/min_compatible/commit/datetime/boot_id/config_version/
    config_crc), decoded on the pc_tools side by
    kilnctrl.devices_safety.parse_safety_response(), not encoded by
    kilnlink_codec.py (which only has the *request*-side encode_announce/
    encode_get_fw_version). Checked below via that decoder instead. Its
    `hostile_vectors` entries are likewise decode-side only -- malformed
    response frames the decoder must reject -- and have no encoder
    counterpart to round-trip, so they are exercised only against the
    decoder, never through kilnlink_codec.py.
  * frame_vectors.json / benchproto_frame_vectors.json -- the outer framing
    layer (header/CRC/stuffing), already mirrored by kilnctrl.protocol per
    kilnlink_codec.py's own module docstring; out of scope for this payload
    codec.
"""
from __future__ import annotations

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import kilnlink_codec  # noqa: E402
from kilnctrl.devices_safety import (  # noqa: E402
    SafetyResponseError,
    parse_safety_response,
)

VECTORS_DIR = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "firmware", "CommonFW", "test", "vectors"
)


def _load(name: str) -> dict:
    with open(os.path.join(VECTORS_DIR, f"{name}_vectors.json"), "r", encoding="utf-8") as fh:
        return json.load(fh)


# name -> encoder, for the "flat" (top-level fields + payload_hex) files.
# "power" is deliberately NOT here -- see test_power_vector_file_v1_body()
# below for why it needs its own comparison, not the generic one.
FLAT_CODECS = {
    "announce": kilnlink_codec.encode_announce,
    "context": kilnlink_codec.encode_context,
    "status": kilnlink_codec.encode_status,
}

# name -> encoder, for the "fields" (nested fields dict + bytes_hex) files.
FIELDS_CODECS = {
    "ceiling": kilnlink_codec.encode_ceiling,
    "clear_trip": kilnlink_codec.encode_clear_trip,
    "diag": kilnlink_codec.encode_diag,
    "get_fw_version": kilnlink_codec.encode_get_fw_version,
    "set_clock": kilnlink_codec.encode_set_clock,
    "trip": kilnlink_codec.encode_trip,
}

_FLAT_NON_FIELD_KEYS = {"name", "note", "payload_hex"}


@pytest.mark.parametrize("vector_name", sorted(FLAT_CODECS))
def test_flat_vector_file_round_trips(vector_name):
    encode = FLAT_CODECS[vector_name]
    doc = _load(vector_name)
    vectors = doc["vectors"]
    assert vectors, f"{vector_name}_vectors.json has no vectors"
    for vec in vectors:
        fields = {k: v for k, v in vec.items() if k not in _FLAT_NON_FIELD_KEYS}
        expected = bytes.fromhex(vec["payload_hex"])
        actual = encode(fields)
        assert actual == expected, (
            f"{vector_name}_vectors.json vector {vec['name']!r}: "
            f"got {actual.hex()}, expected {vec['payload_hex']}"
        )


@pytest.mark.parametrize("vector_name", sorted(FIELDS_CODECS))
def test_fields_vector_file_round_trips(vector_name):
    encode = FIELDS_CODECS[vector_name]
    doc = _load(vector_name)
    vectors = doc["vectors"]
    assert vectors, f"{vector_name}_vectors.json has no vectors"
    for vec in vectors:
        expected = bytes.fromhex(vec["bytes_hex"])
        actual = encode(vec["fields"])
        assert actual == expected, (
            f"{vector_name}_vectors.json vector {vec['name']!r}: "
            f"got {actual.hex()}, expected {vec['bytes_hex']}"
        )


def test_fw_version_response_vectors_decode_via_devices_safety():
    """fw_version_vectors.json's payload_hex is the Pico->ESP response frame
    -- decoded (not encoded) on the pc_tools side, by
    devices_safety.parse_safety_response(). Every "vectors" entry must decode
    to exactly the field values recorded in the JSON."""
    doc = _load("fw_version")
    for vec in doc["vectors"]:
        payload = bytes.fromhex(vec["payload_hex"])
        subcommand, decoded = parse_safety_response(payload)
        assert subcommand == 0x0B
        assert decoded.protocol_version == vec["protocol_version"]
        assert decoded.min_compatible == vec["min_compatible"]
        assert decoded.dirty == bool(vec["dirty"])
        assert decoded.commit == vec["commit"]
        assert decoded.built == vec["datetime"]
        assert decoded.boot_id == vec["boot_id"]
        assert decoded.config_version == vec["config_version"]
        assert decoded.config_crc == vec["config_crc"]


def test_fw_version_response_hostile_vectors_all_rejected():
    """Every hostile_vectors entry names a distinct malformed-input reason
    (TOO_SHORT/WRONG_CMD/LENGTH_MISMATCH/STRING_TOO_LONG); the C side rejects
    all of them, so the Python decoder must reject all of them too, even
    though it does not necessarily spell the reason identically."""
    doc = _load("fw_version")
    for vec in doc["hostile_vectors"]:
        payload = bytes.fromhex(vec["payload_hex"])
        with pytest.raises(SafetyResponseError):
            parse_safety_response(payload)


# --- SAFETY_CMD_POWER (0x0E): V1/V2 coverage, Opus review of --------------
# 51c084f/c49bb0e, finding 7. power_vectors.json predates the 2026-09-06 V2
# extension (firmware/CommonFW/src/kilnlink_power.c: 61 bytes, +3x u16 LE
# counts_avg, KILNLINK_POWER_FLAG_COUNTS_VALID always forced on) and only
# records the 55-byte V1 body, so it cannot be run through the generic
# FLAT_CODECS loop above -- kilnlink_codec.encode_power() now always
# produces 61 bytes with the flag set, matching the real encoder.


def test_power_vector_file_v1_body():
    """The V1 (first 55) bytes of encode_power()'s output must still match
    power_vectors.json's payload_hex, EXCEPT for the flags byte (index 2),
    which the real C encoder (and now this mirror) always ORs
    KILNLINK_POWER_FLAG_COUNTS_VALID (0x08) into."""
    doc = _load("power")
    for vec in doc["vectors"]:
        fields = {k: v for k, v in vec.items() if k not in _FLAT_NON_FIELD_KEYS}
        expected_v1 = bytearray(bytes.fromhex(vec["payload_hex"]))
        expected_v1[2] |= kilnlink_codec.KILNLINK_POWER_FLAG_COUNTS_VALID
        actual = kilnlink_codec.encode_power(fields)
        assert len(actual) == kilnlink_codec.KILNLINK_POWER_LEN_V2, (
            f"power_vectors.json vector {vec['name']!r}: encode_power() must always emit "
            f"{kilnlink_codec.KILNLINK_POWER_LEN_V2} (V2) bytes"
        )
        assert actual[:55] == bytes(expected_v1), (
            f"power_vectors.json vector {vec['name']!r}: V1 body mismatch (with COUNTS_VALID forced on): "
            f"got {actual[:55].hex()}, expected {bytes(expected_v1).hex()}"
        )
        assert actual[2] & kilnlink_codec.KILNLINK_POWER_FLAG_COUNTS_VALID, (
            f"power_vectors.json vector {vec['name']!r}: COUNTS_VALID must always be set"
        )
        # No counts_avg field in this older vector file -- must default to zero.
        assert actual[55:] == b"\x00\x00\x00\x00\x00\x00", (
            f"power_vectors.json vector {vec['name']!r}: counts_avg tail must default to zero"
        )


def test_power_v2_round_trips_through_decode():
    """encode_power() -> decode_power() must recover the exact fields for a
    real counts_avg payload -- the round-trip this pass added decode_power()
    for in the first place."""
    doc = _load("power")
    vec = doc["vectors"][0]
    fields = {k: v for k, v in vec.items() if k not in _FLAT_NON_FIELD_KEYS}
    fields = dict(fields)
    fields["counts_avg"] = [10, 2000, 4095]
    encoded = kilnlink_codec.encode_power(fields)
    assert len(encoded) == kilnlink_codec.KILNLINK_POWER_LEN_V2
    decoded = kilnlink_codec.decode_power(encoded)
    assert decoded["counts_avg"] == [10, 2000, 4095]
    assert decoded["power_window_s"] == fields["power_window_s"]
    assert decoded["flags"] & kilnlink_codec.KILNLINK_POWER_FLAG_COUNTS_VALID
    assert decoded["p_total_w"] == pytest.approx(fields["p_total_w"])
    assert decoded["energy_wh"] == pytest.approx(fields["energy_wh"])
    for got, want in zip(decoded["i_conducting_a"], fields["i_conducting_a"]):
        assert got == pytest.approx(want)
    for got, want in zip(decoded["conduction_fraction"], fields["conduction_fraction"]):
        assert got == pytest.approx(want)
    for got, want in zip(decoded["p_avg_w"], fields["p_avg_w"]):
        assert got == pytest.approx(want)


def test_power_v1_55_byte_frame_decodes_with_zeroed_counts():
    """A legacy 55-byte V1 frame (the exact bytes power_vectors.json already
    records, before this pass's V2 tail existed) must still decode cleanly,
    with counts_avg zeroed rather than raising or reading garbage --
    mirrors kilnlink_power_decode()'s own V1 handling (test_power.c)."""
    doc = _load("power")
    vec = doc["vectors"][0]
    v1_payload = bytes.fromhex(vec["payload_hex"])
    assert len(v1_payload) == kilnlink_codec.KILNLINK_POWER_LEN_V1
    decoded = kilnlink_codec.decode_power(v1_payload)
    assert decoded["counts_avg"] == [0, 0, 0]
    assert decoded["power_window_s"] == vec["power_window_s"]
    assert decoded["p_total_w"] == pytest.approx(vec["p_total_w"])


def test_power_v2_frame_with_counts_valid_clear_decodes_zeroed():
    """A 61-byte frame whose COUNTS_VALID bit is clear (untrusted input, or
    a peer that sends V2-length frames without ever setting the bit) must
    decode with counts_avg zeroed, never the raw tail bytes -- mirrors the
    C decoder's explicit 'never trust memory/bytes you didn't validate'
    discipline (test_power.c's own coverage of this exact case)."""
    doc = _load("power")
    vec = doc["vectors"][0]
    fields = {k: v for k, v in vec.items() if k not in _FLAT_NON_FIELD_KEYS}
    fields = dict(fields)
    fields["flags"] = 0  # base flags, before encode_power() forces COUNTS_VALID on
    fields["counts_avg"] = [10, 2000, 4095]
    encoded = bytearray(kilnlink_codec.encode_power(fields))
    encoded[2] &= ~kilnlink_codec.KILNLINK_POWER_FLAG_COUNTS_VALID  # clear the bit post-encode
    decoded = kilnlink_codec.decode_power(bytes(encoded))
    assert decoded["counts_avg"] == [0, 0, 0]


def test_power_negative_wrong_length_and_cmd():
    """NEGATIVE TEST (negative-test-every-check discipline): decode_power()
    must reject a length that is neither V1 nor V2, and a well-formed-length
    buffer with the wrong command byte -- proves the length/cmd checks
    actually gate, not just that the happy path returns something."""
    doc = _load("power")
    vec = doc["vectors"][0]
    fields = {k: v for k, v in vec.items() if k not in _FLAT_NON_FIELD_KEYS}
    good = bytearray(kilnlink_codec.encode_power(fields))

    with pytest.raises(ValueError):
        kilnlink_codec.decode_power(bytes(good) + b"\x00")  # 62 bytes, neither V1 nor V2

    with pytest.raises(ValueError):
        kilnlink_codec.decode_power(bytes(good[:60]))  # 60 bytes, neither V1 nor V2

    wrong_cmd = bytearray(good)
    wrong_cmd[0] = 0x01  # GET_STATUS's cmd id, not POWER's 0x0E
    with pytest.raises(ValueError):
        kilnlink_codec.decode_power(bytes(wrong_cmd))
