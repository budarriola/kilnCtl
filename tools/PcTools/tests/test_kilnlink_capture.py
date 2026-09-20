"""Tests for kilnctrl.kilnlink_capture -- the Saleae-capture kilnlink frame
decoder (tools/PcTools/TODO.md capability 2).

Fixtures under tests/fixtures/kilnlink/ are synthetic captures built by
build_fixtures.py using kilnctrl.protocol.Frame + kilnctrl.kilnlink_codec's
own encoders -- no real board or Saleae hardware needed or claimed. One
fixture (corrupted_midstream.bin) has a byte deliberately flipped mid-stream
plus injected garbage bytes, to non-vacuously exercise the resync path: it
is not enough for the decoder to detect a bad CRC in isolation, it has to
recover and keep decoding the frames after it.
"""
from __future__ import annotations

import pathlib
import re

import pytest

from kilnctrl import kilnlink_capture as kc
from kilnctrl.protocol import Frame, MsgType, crc16_ccitt_false

FIXTURES = pathlib.Path(__file__).parent / "fixtures" / "kilnlink"


def test_load_bytes_roundtrip(tmp_path):
    data = b"\x01\x02\x03"
    p = tmp_path / "raw.bin"
    p.write_bytes(data)
    assert kc.load_bytes(p) == data


def test_load_saleae_csv_hex_and_decimal(tmp_path):
    csv_text = (
        "Time [s],Value,Parity Error,Framing Error\n"
        "0.0001,0x7E,,\n"
        "0.0002,1,,\n"
        "0.0003,0xFF,,\n"
        "0.0004,,,\n"  # blank Value row must be skipped, not crash
    )
    p = tmp_path / "cap.csv"
    p.write_text(csv_text)
    assert kc.load_saleae_csv(p) == bytes([0x7E, 1, 0xFF])


def test_load_saleae_csv_missing_value_column_raises(tmp_path):
    p = tmp_path / "bad.csv"
    p.write_text("Time [s],Something Else\n0.1,3\n")
    with pytest.raises(ValueError, match="Value"):
        kc.load_saleae_csv(p)


# ---------------------------------------------------------------------------
# Clean, well-formed capture
# ---------------------------------------------------------------------------


def test_clean_mixed_fixture_all_frames_ok():
    data = kc.load_bytes(FIXTURES / "clean_mixed.bin")
    records = kc.decode_capture(data)
    assert len(records) == 5
    assert all(r.ok for r in records), [r.summary() for r in records if not r.ok]

    names = [r.cmd_name for r in records]
    assert names == [
        "PUSH_CONTEXT",
        "GET_STATUS / STATUS (Frame A)",
        "POWER (Frame E)",
        "LOG",
        "CLEAR_TRIP",
    ]

    context = records[0]
    assert context.decoded["boot_id"] == 3
    assert context.decoded["zone_count"] == 1
    assert context.decoded["zones"][0]["setpoint_c"] == pytest.approx(250.0)

    status = records[1]
    assert status.decoded["safety_tc_c"] == pytest.approx(249.1)
    assert status.src_device == kc.LinkDevice.SAFETY
    assert status.dst_device == kc.LinkDevice.ESP

    power = records[2]
    assert power.decoded["mains_voltage_v"] == pytest.approx(240.0)
    assert power.decoded["counts_avg"] == [2500, 25, 25]

    log = records[3]
    assert log.decoded["message"] == "link up"

    clear_trip = records[4]
    assert clear_trip.decoded["trip_mask"] == 0x0020

    # Offsets are monotonic and each frame's span is inside the stream.
    for r in records:
        assert 0 <= r.offset < r.end_offset <= len(data)

    timeline = kc.format_timeline(records)
    assert "5/5 frame(s) decoded cleanly" in timeline
    assert "PUSH_CONTEXT" in timeline


def test_empty_capture():
    assert kc.decode_capture(b"") == []
    assert "empty capture" in kc.format_timeline([])


# ---------------------------------------------------------------------------
# Malformed / partial frames must be reported, never silently dropped, and
# decoding must resynchronise afterward.
# ---------------------------------------------------------------------------


def test_corrupted_midstream_fixture_resyncs():
    data = kc.load_bytes(FIXTURES / "corrupted_midstream.bin")
    records = kc.decode_capture(data)

    # Frame 1 (SET_FIRING_CEILING) and the injected garbage and frame 3
    # (CLEAR_TRIP) are separate records from the corrupted STATUS frame.
    oks = [r for r in records if r.ok]
    bads = [r for r in records if not r.ok]

    assert len(oks) == 2, [r.summary() for r in oks]
    assert oks[0].cmd_name == "SET_FIRING_CEILING"
    assert oks[1].cmd_name == "CLEAR_TRIP"
    assert oks[1].decoded["trip_mask"] == 0x0002

    # At least one bad record is the CRC failure, and at least one is the
    # injected non-frame garbage -- both reported with an offset and an
    # explicit reason, not skipped.
    assert any("CRC mismatch" in (r.error or "") for r in bads)
    # The 6 injected garbage bytes sit BETWEEN two delimiters, so the
    # decoder treats that span as a too-short candidate frame rather than
    # "no enclosing delimiter" -- either way it must be reported, not
    # skipped, which is what this asserts.
    assert any("too short for an 8-byte header" in (r.error or "") for r in bads)
    for r in bads:
        assert r.offset is not None
        assert r.error

    # Decoding did not stop at the first failure: the frame AFTER both the
    # CRC failure and the garbage still decoded.
    assert records[-1].ok
    assert records[-1].cmd_name == "CLEAR_TRIP"

    timeline = kc.format_timeline(records)
    assert "MALFORMED" in timeline
    assert "2/" in timeline  # "2/N frame(s) decoded cleanly"


def test_truncated_tail_fixture_reports_partial_frame_not_silence():
    data = kc.load_bytes(FIXTURES / "truncated_tail.bin")
    records = kc.decode_capture(data)
    assert len(records) == 2
    assert records[0].ok
    assert not records[1].ok
    assert records[1].offset > records[0].offset
    # A capture that stopped mid-frame must be reported as TRUNCATED
    # specifically -- not as the same "non-frame data" record that bytes
    # before the FIRST delimiter get. They are different diagnoses.
    assert "TRUNCATED" in records[1].error
    assert "capture ends mid-frame" in records[1].error
    assert records[1].end_offset == len(data)


def test_garbage_before_first_delimiter_is_reported():
    good = Frame(
        msg_type=MsgType.BROADCAST, msg_index=1, src_device=0, src_task=7,
        dst_device=2, dst_task=7, payload=b"\x77trip",  # 0x77: unrecognised cmd id
    ).to_wire()
    data = b"\x00\x01\x02" + good
    records = kc.decode_capture(data)
    assert not records[0].ok
    assert records[0].offset == 0
    assert records[0].end_offset == 3
    assert "non-frame data" in records[0].error
    assert records[1].ok


def test_back_to_back_delimiters_are_not_reported_as_errors():
    """Two adjacent 0x7E bytes with nothing between them is normal receiver
    noise (a delimiter both closing one frame and opening the next, or a
    stray extra delimiter), per kilnlink_frame.c -- must not appear as a
    malformed record at all."""
    good = Frame(
        msg_type=MsgType.BROADCAST, msg_index=1, src_device=0, src_task=7,
        dst_device=2, dst_task=7, payload=b"\x77trip",  # 0x77: unrecognised cmd id
    ).to_wire()
    # Insert an extra delimiter right after the first frame's closing one.
    data = good[:-1] + b"\x7e" + good[-1:]
    records = kc.decode_capture(data)
    assert len(records) == 1
    assert records[0].ok


def test_length_byte_exceeds_max_payload_reported_without_crashing():
    raw = bytes([MsgType.BROADCAST, 0, 1, 0, 7, 2, 7, 0xFF]) + b"\x00" * 2
    stuffed = bytes([0x7E]) + raw + bytes([0x7E])
    records = kc.decode_capture(stuffed)
    assert len(records) == 1
    assert not records[0].ok
    assert "exceeds max payload" in records[0].error


def test_unterminated_escape_reported():
    good = Frame(
        msg_type=MsgType.BROADCAST, msg_index=1, src_device=0, src_task=7,
        dst_device=2, dst_task=7, payload=b"\x22trip",
    ).to_wire()
    # Corrupt: end the "frame" on a bare trailing escape byte.
    data = good[:-1] + b"\x7d" + b"\x7e"
    records = kc.decode_capture(data)
    assert not records[-1].ok
    assert "escape" in records[-1].error


def test_crc_failure_reports_expected_and_actual():
    payload = b"\x09" + b"\x00\x00\xa0\x44"  # SET_FIRING_CEILING, 1280.0f
    raw = bytes([MsgType.BROADCAST, 0, 1, 0, 7, 2, 7, len(payload)]) + payload
    crc = crc16_ccitt_false(raw)
    corrupted_raw = raw + bytes([((crc >> 8) ^ 0xFF) & 0xFF, crc & 0xFF])
    from kilnctrl.protocol import stuff

    stuffed = stuff(corrupted_raw)
    records = kc.decode_capture(stuffed)
    assert len(records) == 1
    assert not records[0].ok
    assert "CRC mismatch" in records[0].error
    assert records[0].crc_expected == crc
    assert records[0].crc_actual is not None
    assert records[0].crc_actual != crc


def test_unknown_msg_type_reported():
    payload = b"\x09\x00\x00\xa0\x44"
    raw = bytes([0x99, 0, 1, 0, 7, 2, 7, len(payload)]) + payload
    crc = crc16_ccitt_false(raw)
    raw += bytes([(crc >> 8) & 0xFF, crc & 0xFF])
    from kilnctrl.protocol import stuff

    stuffed = stuff(raw)
    records = kc.decode_capture(stuffed)
    assert not records[0].ok
    assert "unknown msg type" in records[0].error


def test_unrecognised_cmd_byte_reports_raw_hex_not_error():
    payload = bytes([0x77, 0xAA, 0xBB])
    raw = bytes([MsgType.BROADCAST, 0, 1, 0, 7, 2, 7, len(payload)]) + payload
    crc = crc16_ccitt_false(raw)
    raw += bytes([(crc >> 8) & 0xFF, crc & 0xFF])
    from kilnctrl.protocol import stuff

    records = kc.decode_capture(stuff(raw))
    assert records[0].ok
    assert records[0].cmd_name == "UNKNOWN(0x77)"
    assert records[0].decoded == {"raw_hex": payload.hex()}


def test_known_cmd_wrong_length_reports_decode_error_with_offset():
    # SET_FIRING_CEILING must be 5 bytes; give it 3.
    payload = bytes([0x09, 0x01, 0x02])
    raw = bytes([MsgType.BROADCAST, 0, 1, 0, 7, 2, 7, len(payload)]) + payload
    crc = crc16_ccitt_false(raw)
    raw += bytes([(crc >> 8) & 0xFF, crc & 0xFF])
    from kilnctrl.protocol import stuff

    records = kc.decode_capture(stuff(raw))
    assert not records[0].ok
    assert "SET_FIRING_CEILING" in records[0].error
    # offset 1: the byte just after the opening 0x7E delimiter -- the actual
    # frame content, which is what a human resyncing by hand needs to see.
    assert records[0].offset == 1


# ---------------------------------------------------------------------------
# Per-command payload decoders, exercised directly (not just through a full
# frame) so a length-format mismatch against kilnlink_codec's own encoders
# is caught precisely, not just "the whole frame failed somehow".
# ---------------------------------------------------------------------------


def test_decode_diag_frame_b_matches_encoder_fields():
    from kilnctrl import kilnlink_codec as codec

    fields = {
        "trip_reason": 6,
        "warn_mask": 0x0004,
        "trip_mask": 0x0020,
        "uptime_ms": 999,
        "boot_reason": "KILNLINK_DIAG_BOOT_POWERON",
        "context_age_100ms": 3,
        "context_frames_ok": 100,
        "context_frames_bad": 1,
        "tx_frames_dropped": 0,
        "state": "KILNLINK_DIAG_STATE_TRIPPED",
        "flags": "KILNLINK_DIAG_FLAG_CALIBRATION_MISSING",
    }
    payload = codec.encode_diag(fields)
    decoded = kc.decode_payload(7, 7, payload)[1]
    assert decoded["trip_reason"] == 6
    assert decoded["trip_mask"] == 0x0020
    assert decoded["state"] == 4  # KILNLINK_DIAG_STATE_TRIPPED
    assert decoded["flags"] == 0x02


def test_decode_trip_frame_d_matches_encoder_fields():
    from kilnctrl import kilnlink_codec as codec

    fields = {
        "trip_seq": 5,
        "trip_reason": 6,
        "uptime_ms": 12345,
        "safety_tc_c": 999.0,
        "deciding_threshold": 950.0,
        "current_a": [1.1, 2.2, 3.3],
        "relay_recent_mask": 0b111,
        "context_age_100ms": 2,
    }
    payload = codec.encode_trip(fields)
    decoded = kc.decode_payload(7, 7, payload)[1]
    assert decoded["trip_seq"] == 5
    assert decoded["trip_reason"] == 6
    assert decoded["current_a"] == pytest.approx([1.1, 2.2, 3.3])
    assert decoded["relay_recent_mask"] == 0b111


def test_decode_announce_version():
    from kilnctrl import kilnlink_codec as codec

    fields = {
        "protocol_version": 12,
        "min_compatible": 11,
        "dirty": 0,
        "commit": "abc1234",
        "datetime": "2026-09-19T00:00:00",
        "boot_id": 9,
    }
    payload = codec.encode_announce(fields)
    name, decoded, err = kc.decode_payload(7, 7, payload)
    assert err is None
    assert name == "ANNOUNCE_VERSION"
    assert decoded["protocol_version"] == 12
    assert decoded["commit"] == "abc1234"
    assert decoded["config_version"] is None  # ANNOUNCE_VERSION has no config fields


def test_decode_get_fw_version_request_is_bare():
    from kilnctrl import kilnlink_codec as codec

    payload = codec.encode_get_fw_version({})
    name, decoded, err = kc.decode_payload(7, 7, payload)
    assert err is None
    assert name == "GET_FW_VERSION (request)"
    assert decoded == {}


def test_decode_set_clock():
    from kilnctrl import kilnlink_codec as codec

    payload = codec.encode_set_clock({"epoch_ms": 1_800_000_000_000})
    name, decoded, err = kc.decode_payload(7, 7, payload)
    assert err is None
    assert decoded["epoch_ms"] == 1_800_000_000_000


def test_decode_status_frame_a_all_three_lengths():
    base = struct_pack_status()
    for extra, expect in ((b"", {}), (b"\x02", {"tx_dropped_sat": 2}),
                          (b"\x02\x01\x03", {"tx_dropped_sat": 2, "flags2": 1, "borrowed": True, "borrowed_zone_index": 3})):
        payload = base + extra
        name, decoded, err = kc.decode_payload(7, 7, payload)
        assert err is None, err
        for k, v in expect.items():
            assert decoded[k] == v


def struct_pack_status() -> bytes:
    from kilnctrl import kilnlink_codec as codec

    return codec.encode_status(
        {
            "flags": 0,
            "safety_tc_c": 1.0,
            "cold_junction_c": 2.0,
            "tc_fault": 0,
            "current1_a": 0.0,
            "current2_a": 0.0,
            "current3_a": 0.0,
        }
    )


def test_decode_ct_cal_and_auto_zero():
    payload = bytes([0x1A]) + b"".join(
        __import__("struct").pack("<Bff", 1, 100.0 + i, 0.5 * i) for i in range(3)
    )
    name, decoded, err = kc.decode_payload(7, 7, payload)
    assert err is None
    assert len(decoded["channels"]) == 3
    assert decoded["channels"][0]["gain"] == pytest.approx(100.0)

    az_payload = __import__("struct").pack("<BBBHHH", 0x28, 2, 1, 50, 200, 12345)
    name, decoded, err = kc.decode_payload(7, 7, az_payload)
    assert err is None
    assert decoded["channel"] == 1
    assert decoded["zero_counts"] == 12345


def test_decode_rollback_and_reboot_result():
    for cmd, name_expect in ((0x25, "ROLLBACK_RESULT"), (0x2A, "REBOOT_RESULT")):
        payload = bytes([cmd, 1, 7])
        name, decoded, err = kc.decode_payload(7, 7, payload)
        assert err is None
        assert name_expect in name
        assert decoded == {"accepted": True, "reason": 7}


def test_decode_inject_tc():
    # fault_bits is uint8_t in kilnlink_inject_tc.h -- a mask with bit 7 set
    # must read as 0xFE, never as -2 (it is a SAFETY_THERMO_FAULT_* bitfield).
    payload = __import__("struct").pack("<BBffB", 0x21, 1, 123.4, 25.0, 0xFE)
    name, decoded, err = kc.decode_payload(7, 7, payload)
    assert err is None
    assert decoded["valid"] is True
    assert decoded["tc_c"] == pytest.approx(123.4)
    assert decoded["fault_bits"] == 0xFE


def test_decode_log_unknown_level_falls_back_to_hex():
    payload = bytes([0xEE]) + b"hi"
    name, decoded, err = kc.decode_payload(5, 7, payload)
    assert err is None
    assert name == "LOG"
    assert decoded["level_name"] == "0xEE"
    assert decoded["message"] == "hi"


# ---------------------------------------------------------------------------
# Drift guards against the C, which is authoritative for this wire format.
#
# This module is the FOURTH consumer of the kilnlink wire format (the two
# firmwares, kilnctrl.protocol, and this decoder). The three tests below
# read the firmware headers themselves rather than a prose doc, and fail
# hard -- never skip -- if a header is missing, so they cannot go quietly
# vacuous the way a `if not path.is_file(): skipTest(...)` guard would.
# ---------------------------------------------------------------------------

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
KILNLINK_INCLUDE = REPO_ROOT / "firmware" / "CommonFW" / "include" / "kilnlink"
UART_PROTOCOL_H = (
    REPO_ROOT / "firmware" / "hwAbstraction" / "esp" / "uart" / "uart_protocol.h"
)


def test_cmd_names_covers_every_kilnlink_cmd_id_defined_in_firmware():
    assert KILNLINK_INCLUDE.is_dir(), f"missing firmware headers at {KILNLINK_INCLUDE}"
    pattern = re.compile(r"#define\s+(KILNLINK_[A-Z0-9_]*CMD[A-Z0-9_]*)\s+0x([0-9A-Fa-f]+)u?")
    defined: dict[int, set] = {}
    for header in sorted(KILNLINK_INCLUDE.glob("*.h")):
        for name, hexval in pattern.findall(header.read_text(encoding="utf-8", errors="replace")):
            defined.setdefault(int(hexval, 16), set()).add(name)
    assert defined, "parsed no KILNLINK_*_CMD defines -- the regex or the headers moved"
    missing = {f"0x{cid:02X}": sorted(names) for cid, names in defined.items()
               if cid not in kc.CMD_NAMES}
    assert not missing, (
        "kilnlink_capture.CMD_NAMES is missing firmware command id(s); they would "
        f"decode as UNKNOWN(0x..) in a capture timeline: {missing}"
    )


def test_link_device_enum_matches_uart_proto_device_t():
    assert UART_PROTOCOL_H.is_file(), f"missing firmware header at {UART_PROTOCOL_H}"
    text = UART_PROTOCOL_H.read_text(encoding="utf-8", errors="replace")
    found = dict(
        (name, int(value))
        for name, value in re.findall(r"UART_PROTO_DEVICE_([A-Z]+)\s*=\s*(\d+)", text)
    )
    assert found, "parsed no UART_PROTO_DEVICE_* values from uart_protocol.h"
    assert found == {d.name: int(d) for d in kc.LinkDevice}, (
        f"LinkDevice {[(d.name, int(d)) for d in kc.LinkDevice]} has drifted from "
        f"uart_proto_device_t {found}"
    )
    # The reason this enum is local at all: protocol.Device deliberately omits
    # SAFETY, and test_link_hub_routing.py pins that omission. If that ever
    # changes, this local enum should be retired rather than left to drift.
    from kilnctrl.protocol import Device

    assert "SAFETY" not in Device.__members__


def test_task_ids_are_the_protocol_modules_own_not_a_local_copy():
    from kilnctrl import protocol

    assert kc.UART_TASK_ID_LOG is protocol.UART_TASK_ID_LOG
    assert kc.UART_TASK_ID_SAFETY is protocol.UART_TASK_ID_SAFETY


# ---------------------------------------------------------------------------
# Decode the SHARED CommonFW vectors -- the same JSON the C host tests and
# kilnlink_codec.py are checked against. This is what makes "the decoders are
# the exact inverse of the C encoders" a measured claim rather than an
# inference from the Python encoders alone (those could be wrong in the same
# direction as the decoder and the round-trip would still close).
# ---------------------------------------------------------------------------

VECTORS_DIR = REPO_ROOT / "firmware" / "CommonFW" / "test" / "vectors"

#: vector file -> (field container key or None for flat, bytes key)
_VECTOR_FILES = {
    "status_vectors.json": (None, "payload_hex"),
    "context_vectors.json": (None, "payload_hex"),
    "fw_version_vectors.json": (None, "payload_hex"),
    "diag_vectors.json": ("fields", "bytes_hex"),
    "trip_vectors.json": ("fields", "bytes_hex"),
    "ceiling_vectors.json": ("fields", "bytes_hex"),
    "clear_trip_vectors.json": ("fields", "bytes_hex"),
    "set_clock_vectors.json": ("fields", "bytes_hex"),
    "set_config_vectors.json": ("fields", "bytes_hex"),
    "get_fw_version_vectors.json": ("fields", "bytes_hex"),
}

#: Fields whose vector spelling is not the decoder's (enum names vs numbers,
#: a different key, or a value this decoder deliberately does not surface).
_VECTOR_SKIP_FIELDS = {"name", "note", "_comment", "boot_reason", "state", "flags",
                       "zones", "zone_count", "current_a", "dirty"}


@pytest.mark.parametrize("filename", sorted(_VECTOR_FILES))
def test_decoders_match_commonfw_vectors(filename):
    import json

    path = VECTORS_DIR / filename
    assert path.is_file(), f"missing shared CommonFW vector file {path}"
    container_key, bytes_key = _VECTOR_FILES[filename]
    doc = json.loads(path.read_text(encoding="utf-8"))
    vectors = doc["vectors"]
    assert vectors, f"{filename} has no vectors"

    for vec in vectors:
        payload = bytes.fromhex(vec[bytes_key])
        name, decoded, err = kc.decode_payload(
            kc.UART_TASK_ID_SAFETY, kc.UART_TASK_ID_SAFETY, payload
        )
        assert err is None, f"{filename}:{vec.get('name')} failed to decode: {err}"
        assert decoded is not None
        fields = vec[container_key] if container_key else vec
        compared = 0
        for key, expected in fields.items():
            if key in _VECTOR_SKIP_FIELDS or key == bytes_key or key not in decoded:
                continue
            actual = decoded[key]
            if isinstance(expected, str) and expected in ("NaN", "Infinity", "-Infinity"):
                expected = float(expected.replace("Infinity", "inf"))
            if isinstance(expected, float):
                assert actual == pytest.approx(expected, nan_ok=True), (
                    f"{filename}:{vec.get('name')}: {key} {actual} != {expected}"
                )
            else:
                assert actual == expected, (
                    f"{filename}:{vec.get('name')}: {key} {actual!r} != {expected!r}"
                )
            compared += 1
        # Non-vacuity: a vector that compared nothing would pass silently.
        if len(payload) > 1:
            assert compared > 0, f"{filename}:{vec.get('name')} compared no fields"
