"""Builds the binary kilnlink capture fixtures in this directory.

Not a test itself, and not run automatically -- these fixtures are committed
binary files, generated once by this script and checked in like any other
test vector, per the same pattern as tests/fixtures used elsewhere in
PcTools. Re-run manually (``python build_fixtures.py`` from this directory)
only if a fixture needs to be regenerated deliberately.

Uses kilnctrl.protocol.Frame (framing) + kilnctrl.kilnlink_codec (payload
encoders) -- the same two modules kilnlink_capture.py's decoder is checked
against -- so a fixture is guaranteed protocol-correct by construction,
never hand-crafted bytes that only look right.
"""
import pathlib
import sys

_SRC = pathlib.Path(__file__).resolve().parents[3] / "src"
sys.path.insert(0, str(_SRC))

from kilnctrl import kilnlink_codec as codec
from kilnctrl.protocol import Frame, MsgType

HERE = pathlib.Path(__file__).parent

ESP, HOST, SAFETY = 0, 1, 2
TASK_SAFETY = 7
TASK_LOG = 5


def _frame(src_device, src_task, dst_device, dst_task, payload: bytes, msg_index=1) -> bytes:
    return Frame(
        msg_type=MsgType.BROADCAST,
        msg_index=msg_index,
        src_device=src_device,
        src_task=src_task,
        dst_device=dst_device,
        dst_task=dst_task,
        payload=payload,
    ).to_wire()


def build_clean_mixed() -> bytes:
    """A short, all-valid timeline: ESP->Pico PUSH_CONTEXT, Pico->ESP STATUS
    (Frame A, V2-with-borrow-flags length), Pico->ESP POWER (Frame E, V2),
    Pico->ESP a LOG line, ESP->Pico CLEAR_TRIP."""
    out = bytearray()

    context_payload = codec.encode_context(
        {
            "flags": 0x01,
            "boot_id": 3,
            "seq": 42,
            "uptime_ms": 123456,
            "relay_now_mask": 0b101,
            "relay_recent_mask": 0b111,
            "recent_window_s": 30,
            "zone_count": 1,
            "zones": [
                {
                    "zone_index": 0,
                    "flags": 0,
                    "setpoint_c": 250.0,
                    "measured_c": 248.5,
                    "sample_counter": 7,
                    "tc_type": 1,
                    "tc_fault": 0,
                }
            ],
        }
    )
    out += _frame(ESP, TASK_SAFETY, SAFETY, TASK_SAFETY, context_payload, msg_index=1)

    status_payload = codec.encode_status(
        {
            "flags": 0x00,
            "safety_tc_c": 249.1,
            "cold_junction_c": 24.0,
            "tc_fault": 0,
            "current1_a": 1.5,
            "current2_a": 0.0,
            "current3_a": 0.0,
        }
    )
    out += _frame(SAFETY, TASK_SAFETY, ESP, TASK_SAFETY, status_payload, msg_index=2)

    power_payload = codec.encode_power(
        {
            "power_window_s": 5,
            "flags": 0,
            "mains_voltage_v": 240.0,
            "i_conducting_a": [1.5, 0.0, 0.0],
            "conduction_fraction": [0.6, 0.0, 0.0],
            "p_avg_w": [360.0, 0.0, 0.0],
            "p_total_w": 360.0,
            "energy_wh": 12.3,
            "counts_avg": [2500, 25, 25],
        }
    )
    out += _frame(SAFETY, TASK_SAFETY, ESP, TASK_SAFETY, power_payload, msg_index=3)

    log_payload = bytes([2]) + b"link up"  # LogLevel value 2, message ascii
    out += _frame(SAFETY, TASK_LOG, ESP, TASK_LOG, log_payload, msg_index=4)

    clear_trip_payload = codec.encode_clear_trip({"trip_mask": 0x0020})
    out += _frame(ESP, TASK_SAFETY, SAFETY, TASK_SAFETY, clear_trip_payload, msg_index=5)

    return bytes(out)


def build_corrupted_midstream() -> bytes:
    """Same three-frame skeleton as above, but the SECOND frame (STATUS) has
    a payload byte flipped after framing -- this breaks its CRC without
    breaking the surrounding delimiters, and 6 stray non-frame bytes are
    injected between frame 2 and frame 3 to also exercise the "garbage
    between two valid delimiters" resync path. Frames 1 and 3 must still
    decode cleanly: this is the fixture that proves resync-after-garbage is
    non-vacuous, not just CRC-failure detection on an otherwise-intact
    stream.
    """
    ceiling_payload = codec.encode_ceiling({"firing_max_c": 1300.0})
    f1 = _frame(ESP, TASK_SAFETY, SAFETY, TASK_SAFETY, ceiling_payload, msg_index=10)

    status_payload = codec.encode_status(
        {
            "flags": 0x00,
            "safety_tc_c": 100.0,
            "cold_junction_c": 22.0,
            "tc_fault": 0,
            "current1_a": 0.0,
            "current2_a": 0.0,
            "current3_a": 0.0,
        }
    )
    f2 = bytearray(_frame(SAFETY, TASK_SAFETY, ESP, TASK_SAFETY, status_payload, msg_index=11))
    # Flip a payload byte inside the delimiters (not the delimiter itself,
    # and not an unescaped 0x7E/0x7D, so framing/resync is unaffected --
    # only the CRC check inside this one frame should fail).
    corrupt_at = len(f2) // 2
    while f2[corrupt_at] in (0x7E, 0x7D):
        corrupt_at += 1
    f2[corrupt_at] ^= 0xFF

    garbage = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])

    trip_payload = codec.encode_clear_trip({"trip_mask": 0x0002})
    f3 = _frame(ESP, TASK_SAFETY, SAFETY, TASK_SAFETY, trip_payload, msg_index=12)

    return bytes(f1) + bytes(f2) + garbage + f3


def build_truncated_tail() -> bytes:
    """One valid frame, then a second frame's opening delimiter and partial
    header/payload with no closing delimiter at all -- simulates a capture
    that was stopped mid-frame."""
    ceiling_payload = codec.encode_ceiling({"firing_max_c": 1300.0})
    f1 = _frame(ESP, TASK_SAFETY, SAFETY, TASK_SAFETY, ceiling_payload, msg_index=20)

    full2 = bytearray(_frame(ESP, TASK_SAFETY, SAFETY, TASK_SAFETY, ceiling_payload, msg_index=21))
    # Cut off after the opening delimiter + a few header bytes, no closing
    # delimiter -- a genuine "capture stopped here" tail.
    cut = full2[: len(full2) // 3]
    return bytes(f1) + bytes(cut)


def main() -> None:
    (HERE / "clean_mixed.bin").write_bytes(build_clean_mixed())
    (HERE / "corrupted_midstream.bin").write_bytes(build_corrupted_midstream())
    (HERE / "truncated_tail.bin").write_bytes(build_truncated_tail())
    print("wrote fixtures to", HERE)


if __name__ == "__main__":
    main()
