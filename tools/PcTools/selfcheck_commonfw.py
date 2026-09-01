"""CommonFW cross-implementation vector checks.

Part of the selfcheck.py split (pure refactor) -- moved verbatim, no logic
changes. See selfcheck.py's module docstring for the overall map.
"""
from __future__ import annotations

import json
import math
import pathlib
import struct
import sys
import threading
import time

from kilnctrl import devices, pin_overlay
from kilnctrl.protocol import (
    FRAME_DELIM,
    FRAME_ESC,
    Device,
    Frame,
    FrameDecoder,
    FrameError,
    MsgType,
    crc16_ccitt_false,
    stuff,
    unstuff,
)
from kilnctrl.serial_link import list_ports, recommend_port

from selfcheck_common import check, _make_pair, _responder


def commonfw_vector_checks() -> None:
    """Consumes firmware/CommonFW/test/vectors/frame_vectors.json -- the
    manifest kilnlink's own host test (test/test_frame.c) also asserts
    against. pc_tools is the *third* implementation of this exact envelope
    (CommonFW/README.md); this is what keeps it honest rather than trusting
    that Frame/stuff/protocol.py still agrees with the C side after a change
    on either end.

    Skips cleanly (not a failure) if the manifest doesn't exist yet or this
    checkout doesn't have CommonFW -- the manifest is new as of 2026-08-16
    and this file needs to keep working for anyone on an older checkout.
    """
    vectors_path = (
        pathlib.Path(__file__).resolve().parents[2]
        / "firmware" / "CommonFW" / "test" / "vectors" / "frame_vectors.json"
    )
    if not vectors_path.is_file():
        print(f"\n== CommonFW kilnlink vectors == (skipped: {vectors_path} not found)")
        return

    manifest = json.loads(vectors_path.read_text(encoding="utf-8"))
    print(f"\n== CommonFW kilnlink vectors == ({vectors_path.name})")

    def as_device(value: int):
        # Device only has ESP/HOST -- SAFETY (2) is a real device id on the
        # isolated link but isn't part of this PC-link-facing enum, same
        # leniency Frame.from_raw itself uses (protocol.py's _as_device).
        try:
            return Device(value)
        except ValueError:
            return value

    for v in manifest.get("vectors", []):
        name = v["name"]
        frame = Frame(
            msg_type=MsgType(v["msg_type"]),
            msg_index=v["msg_index"],
            src_device=as_device(v["src_device"]),
            src_task=v["src_task"],
            dst_device=as_device(v["dst_device"]),
            dst_task=v["dst_task"],
            payload=bytes.fromhex(v["payload_hex"]),
        )
        raw = frame.to_raw()
        check(f"{name}: raw_hex matches", raw.hex(), v["raw_hex"])
        check(f"{name}: wire_hex matches", stuff(raw).hex(), v["wire_hex"])
        decoded = Frame.from_raw(bytes.fromhex(v["raw_hex"]))
        check(f"{name}: decode round-trips", decoded, frame)

    for hv in manifest.get("hostile_vectors", []):
        name = hv["name"]
        expect = hv["expect_error"]
        checked_by = hv.get("checked_by", "decode")
        if checked_by == "unstuff":
            # pc_tools' protocol.py unstuff() is documented as a one-shot
            # test/decode convenience, not the live RX path (that's
            # FrameDecoder) -- it does not raise on every case kilnlink's
            # stricter kilnlink_unstuff does. See the vector's own
            # "checked_by_note" in the manifest for which ones apply here.
            if hv.get("checked_by_note", "").startswith("kilnlink only"):
                print(f"  (skip) {name}: kilnlink-only case, see manifest note")
                continue
            try:
                unstuff(bytes.fromhex(hv["wire_hex"]))
                check(f"{name}: raises on {expect}", "did not raise", expect)
            except (FrameError, IndexError, ValueError):
                check(f"{name}: raises on {expect}", True, True)
        else:
            try:
                Frame.from_raw(bytes.fromhex(hv["raw_hex"]))
                check(f"{name}: raises on {expect}", "did not raise", expect)
            except FrameError:
                check(f"{name}: raises on {expect}", True, True)


#: The two vector-file shapes in firmware/CommonFW/test/vectors/. The older
#: ones (context/status/announce/power) put each vector's fields at the top
#: level of the vector object and record the expected bytes as
#: "payload_hex"; the newer ones (diag/trip, and this pass's ceiling/
#: clear_trip/get_fw_version/set_clock) nest fields under "fields" and
#: record expected bytes as "bytes_hex" -- see each file's own
#: "_comment"/"note" for why. Mapping each manifest file name to
#: (kilnlink_codec function, which key holds the field dict -- None means
#: "the vector object itself", which key holds the expected hex) lets one
#: loop below drive every one of them.
_PAYLOAD_VECTOR_MANIFESTS = (
    ("context_vectors.json", "encode_context", None, "payload_hex"),
    ("status_vectors.json", "encode_status", None, "payload_hex"),
    ("announce_vectors.json", "encode_announce", None, "payload_hex"),
    ("power_vectors.json", "encode_power", None, "payload_hex"),
    ("diag_vectors.json", "encode_diag", "fields", "bytes_hex"),
    ("trip_vectors.json", "encode_trip", "fields", "bytes_hex"),
    ("ceiling_vectors.json", "encode_ceiling", "fields", "bytes_hex"),
    ("clear_trip_vectors.json", "encode_clear_trip", "fields", "bytes_hex"),
    ("get_fw_version_vectors.json", "encode_get_fw_version", "fields", "bytes_hex"),
    ("set_clock_vectors.json", "encode_set_clock", "fields", "bytes_hex"),
)


def commonfw_payload_vector_checks() -> None:
    """Consumes every *payload*-codec vector manifest in
    firmware/CommonFW/test/vectors/ (as opposed to commonfw_vector_checks()
    above, which only covers frame_vectors.json, the framing layer) against
    kilnctrl.kilnlink_codec -- pc_tools' pure-Python mirror of the C payload
    encoders. Proves Python produces byte-identical output to
    firmware/CommonFW/src/kilnlink_<name>.c for every codec that has host
    tests and a vectors file, closing the ROADMAP.md M2 gap where
    selfcheck.py only ever consumed the framing layer.

    Skips a manifest cleanly (not a failure) if it doesn't exist yet, same
    convention as commonfw_vector_checks() -- this file needs to keep
    working for anyone on an older checkout that predates a given codec.
    """
    from kilnctrl import kilnlink_codec

    vectors_dir = (
        pathlib.Path(__file__).resolve().parents[2]
        / "firmware" / "CommonFW" / "test" / "vectors"
    )

    for filename, fn_name, fields_key, hex_key in _PAYLOAD_VECTOR_MANIFESTS:
        path = vectors_dir / filename
        if not path.is_file():
            print(f"\n== CommonFW kilnlink payload vectors: {filename} == (skipped: not found)")
            continue

        manifest = json.loads(path.read_text(encoding="utf-8"))
        print(f"\n== CommonFW kilnlink payload vectors: {filename} ==")
        encode = getattr(kilnlink_codec, fn_name)

        for v in manifest.get("vectors", []):
            name = v["name"]
            fields = v[fields_key] if fields_key else v
            expected = v[hex_key]
            got = encode(fields).hex()
            check(f"{filename} {name}: bytes match {hex_key}", got, expected)


