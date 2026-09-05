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
FLAT_CODECS = {
    "announce": kilnlink_codec.encode_announce,
    "context": kilnlink_codec.encode_context,
    "status": kilnlink_codec.encode_status,
    "power": kilnlink_codec.encode_power,
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
