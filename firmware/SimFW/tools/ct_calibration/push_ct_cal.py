#!/usr/bin/env python3
"""Push a bench CT calibration table into SaftyFW's own flash, and verify it
landed by reading it back.

Closes the gap this package's README ("Readback path") named: a bench
calibration run (``calibrate_ct.py``) could compute per-channel
``fit_gain``/``fit_offset`` and write them to a JSON table, but nothing
existed to push those constants into ``SaftyFW``'s own ``config_store.h``
flash record over the wire commands that already exist there
(``SAFETY_CMD_SET_CT_CAL`` 0x19, ``SAFETY_CMD_GET_CT_CAL``/``CT_CAL`` 0x1A --
see ``firmware/CommonFW/include/kilnlink/kilnlink_set_ct_cal.h``,
``kilnlink_get_ct_cal.h``, ``kilnlink_ct_cal.h`` for the authoritative wire
layout and rationale, and ``firmware/SaftyFW/src/config_store.h`` for what
the Pico does with what arrives).

Wire format is re-implemented here in pure Python, byte-for-byte matching
those three CommonFW headers -- there is no Python FFI boundary onto
CommonFW's C sources in this repo, so this is a second, independent
implementation of the same layout, not a wrapper around the first. If those
headers' wire format ever changes, this module's ``_SET_CT_CAL_STRUCT`` /
``_CT_CAL_CHANNEL_STRUCT`` / the length constants below need the matching
change made by hand.

Fit inversion: identical reasoning and identical arithmetic to
``firmware/SimFW/tools/gen_ct_cal_table.py``, reused (not reimplemented) via
that script's ``load_channels()`` -- see this module's ``build_pushes_from_json()``.
``calibration_table.py`` stores the *measurement* fit, ``measured_a =
fit_gain * commanded + fit_offset``; ``config_store.h``'s own header comment
says explicitly that ``SET_CT_CAL``'s sender must invert it before
transmitting so that the Pico's ``corrected = gain * raw + offset`` undoes
the measured error: ``gain = 1 / fit_gain``, ``offset = -fit_offset /
fit_gain``. ``load_channels()`` already does exactly this and already
refuses a non-invertible (zero/NaN) ``fit_gain`` and a table whose
``crosstalk_passed`` is false -- reusing it means this module inherits both
refusals for free instead of re-deriving them and risking the two copies
drifting apart.

**A genuine, currently-unresolved blocker, found while building this, not
manufactured for this docstring.** ``firmware/KilnFW/App/drivers/uart_bridge.c``'s
``safety_bridge_task()`` switches on the SAFETY subcommand byte and has no
``case`` for ``SAFETY_CMD_SET_CT_CAL`` (0x19) or
``SAFETY_CMD_GET_CT_CAL``/``CT_CAL`` (0x1A) -- both fall through to
``default: rejected = true`` and are silently dropped on the ESP (a warning
is logged to the ESP's own console; nothing is returned over the PC UART
link). ``firmware/KilnFW/App/drivers/safety_link.h``/``.c`` (the ESP's
send/receive glue to the isolated RP2040 link) likewise has no
``safety_link_send_set_ct_cal()`` or GET_CT_CAL forwarder -- grepped for at
the time this was written, zero hits in either file. So even though
CommonFW's codecs and SaftyFW's ``config_store.c`` both support these
commands end to end on the Pico side, and this module builds the wire bytes
for them correctly (verified against a fake transport -- see this package's
tests), **a real ESP32 running the KilnFW currently checked into this repo
will ACK a SET_CT_CAL send** (task 7 exists on the ESP and ACKs at the
transport layer regardless of whether the subcommand byte is recognised --
see ``kilnctrl.serial_link``'s own docstring on exactly this "delivered,
acknowledged, silently discarded" shape) **and then silently discard it,
and a GET_CT_CAL query will time out waiting for a reply that is never
sent.** This is a different kind of gap than "no CT hardware exists yet":
it is firmware KilnFW does not have yet, on the ESP side of the link.
Fixing it means adding cases to ``uart_bridge.c``'s ``safety_bridge_task()``
switch and a forwarder in ``safety_link.c`` -- both squarely inside
``firmware/KilnFW/**``, which is out of scope for this task (see this
package's README for the fuller account and what should happen next).

Usage (bench day, once the KilnFW gap above is closed)::

    python push_ct_cal.py --json ct_calibration_table.json \\
        --dut-serial-port COM7

Exit codes: ``0`` pushed and verified, ``1`` table refused (crosstalk not
passed, wrong schema, non-invertible gain -- see ``gen_ct_cal_table.load_channels``),
``3`` could not connect to the kilnctrl link, ``5`` a SET_CT_CAL/GET_CT_CAL
frame could not be delivered or answered, ``6`` the post-push readback did
not match what was sent.
"""

from __future__ import annotations

import argparse
import queue
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Protocol, Sequence

_TOOLS_DIR = Path(__file__).resolve().parent  # ct_calibration/
_SIMFW_TOOLS_DIR = _TOOLS_DIR.parent  # firmware/SimFW/tools/
if str(_SIMFW_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_SIMFW_TOOLS_DIR))

from gen_ct_cal_table import NUM_CHANNELS, TableError, load_channels  # noqa: E402

#: firmware/SimFW/tools -> repo root -> tools/PcTools/src, same computation
#: calibrate_ct.py uses (this file lives in the same directory).
_PCTOOLS_SRC = Path(__file__).resolve().parents[4] / "tools" / "PcTools" / "src"

# --- Wire format -- mirrors kilnlink_set_ct_cal.h / kilnlink_get_ct_cal.h /
# kilnlink_ct_cal.h byte-for-byte. See this module's docstring. --------------

SET_CT_CAL_CMD = 0x19
GET_CT_CAL_CMD = 0x1A
CT_CAL_REPLY_CMD = 0x1A  # shared command id with GET_CT_CAL; distinguished by
                          # direction and length, same convention as GET_FW_VERSION

SET_CT_CAL_LEN = 11  # cmd(1) + channel(1) + calibrated(1) + gain f32(4) + offset f32(4)
GET_CT_CAL_LEN = 1  # cmd(1), no fields
CT_CAL_CHANNEL_LEN = 9  # calibrated u8(1) + gain f32(4) + offset f32(4)
CT_CAL_LEN = 1 + NUM_CHANNELS * CT_CAL_CHANNEL_LEN  # 28

_SET_CT_CAL_STRUCT = struct.Struct("<BBBff")  # cmd, channel, calibrated, gain, offset
_CT_CAL_CHANNEL_STRUCT = struct.Struct("<Bff")  # calibrated, gain, offset

assert _SET_CT_CAL_STRUCT.size == SET_CT_CAL_LEN
assert _CT_CAL_CHANNEL_STRUCT.size == CT_CAL_CHANNEL_LEN


class CtCalWireError(ValueError):
    """A frame this module was asked to encode was malformed (bad channel
    index), or a frame received from the wire does not match the expected
    SAFETY_CMD_CT_CAL shape (wrong length or command byte)."""


class CtCalLinkError(RuntimeError):
    """A SET_CT_CAL send or GET_CT_CAL query could not be completed at the
    transport layer: not delivered (NACK/timeout), or ACKed but never
    answered. Distinct from `VerifyMismatchError`, which means the round
    trip completed but the readback disagreed with what was sent."""


@dataclass(frozen=True)
class ChannelPush:
    """One channel's worth of calibration to push -- always sent explicitly.

    An uncalibrated channel is represented the same way here as everywhere
    else in this feature: `calibrated=False` with `gain=0.0, offset=0.0`,
    never inferred, never silently skipped, and never confused with a
    fabricated gain=1/offset=0 "identity" calibration (see this package's
    README and `config_store.h`'s header comment on exactly this trap).
    """

    channel: int
    calibrated: bool
    gain: float
    offset: float
    provenance: str = ""


def encode_set_ct_cal(push: ChannelPush) -> bytes:
    """Serializes one SET_CT_CAL frame. Refuses (raises `CtCalWireError`) a
    channel outside 0..NUM_CHANNELS-1 -- the same range check
    `kilnlink_set_ct_cal_decode()`'s receiver performs, applied here on the
    sending side too so a bug in this tool fails loudly instead of sending
    a frame SaftyFW would have to reject on its own."""
    if not 0 <= push.channel < NUM_CHANNELS:
        raise CtCalWireError(f"channel {push.channel} outside 0..{NUM_CHANNELS - 1}")
    # An uncalibrated channel's gain/offset are documented as ignored by the
    # receiver (config_store.h), but this module sends literal 0.0/0.0 for
    # them anyway rather than whatever happens to be in `push` -- belt and
    # suspenders against a caller that built a ChannelPush with stale
    # numbers next to calibrated=False.
    gain = push.gain if push.calibrated else 0.0
    offset = push.offset if push.calibrated else 0.0
    return _SET_CT_CAL_STRUCT.pack(
        SET_CT_CAL_CMD, push.channel, 1 if push.calibrated else 0, gain, offset
    )


def encode_get_ct_cal() -> bytes:
    """The one-byte GET_CT_CAL request -- cmd byte only, no fields."""
    return bytes([GET_CT_CAL_CMD])


def decode_ct_cal_reply(payload: bytes) -> "tuple[ChannelPush, ...]":
    """Parses a SAFETY_CMD_CT_CAL reply payload into NUM_CHANNELS `ChannelPush`
    entries, channel 0 first. Raises `CtCalWireError` on anything that
    doesn't match the fixed 28-byte shape -- this is data arriving off a
    UART link and is treated with the same suspicion CommonFW/README.md
    rule 6 asks of the C decoders this mirrors."""
    if len(payload) != CT_CAL_LEN:
        raise CtCalWireError(f"CT_CAL reply wrong length: {len(payload)} != {CT_CAL_LEN}")
    if payload[0] != CT_CAL_REPLY_CMD:
        raise CtCalWireError(
            f"CT_CAL reply wrong cmd byte: 0x{payload[0]:02X} != 0x{CT_CAL_REPLY_CMD:02X}"
        )
    out = []
    pos = 1
    for ch in range(NUM_CHANNELS):
        chunk = payload[pos : pos + CT_CAL_CHANNEL_LEN]
        calibrated, gain, offset = _CT_CAL_CHANNEL_STRUCT.unpack(chunk)
        out.append(ChannelPush(channel=ch, calibrated=bool(calibrated), gain=gain, offset=offset))
        pos += CT_CAL_CHANNEL_LEN
    return tuple(out)


# --- Building the push list from a bench JSON table -------------------------


def build_pushes_from_json(json_path: "Path | str") -> "tuple[ChannelPush, ...]":
    """Loads a bench calibration table and returns exactly NUM_CHANNELS
    `ChannelPush` entries (channel 0..N-1), reusing
    `gen_ct_cal_table.load_channels()` for the inversion arithmetic and its
    refusal gates (schema version, `crosstalk_passed`, non-invertible
    `fit_gain`) rather than reimplementing them -- see this module's
    docstring. Raises `gen_ct_cal_table.TableError` (unchanged) on any of
    those refusals; the caller decides what to do (this package's CLI exits
    1 without ever contacting the link).

    A channel present and passing in the table becomes `calibrated=True`
    with the inverted `(gain, offset)`. A channel absent from the table
    (never swept with `--channels`, or -- impossible today, since
    `calibrate_ct.py` refuses to save a table with any rejected channel --
    swept but rejected) becomes an EXPLICIT `calibrated=False, gain=0.0,
    offset=0.0` push, never a fabricated gain=1/offset=0 standing in for
    "uncalibrated". This is what makes pushing a table where a channel is
    uncalibrated actually leave that channel uncalibrated in flash, end to
    end (this task's honesty/explicitness requirement)."""
    channels = load_channels(Path(json_path))  # TableError propagates unchanged
    pushes = []
    for ch in range(NUM_CHANNELS):
        entry = channels.get(ch)
        if entry is None:
            pushes.append(
                ChannelPush(
                    channel=ch, calibrated=False, gain=0.0, offset=0.0,
                    provenance="no bench data for this channel -- uncalibrated",
                )
            )
        else:
            gain, offset, prov = entry
            pushes.append(ChannelPush(channel=ch, calibrated=True, gain=gain, offset=offset, provenance=prov))
    return tuple(pushes)


# --- Transport ---------------------------------------------------------------


class CtCalTransport(Protocol):
    """Everything `push_and_verify()` needs from a link: send one raw
    SET_CT_CAL frame, and issue one raw GET_CT_CAL query, getting back the
    raw 28-byte CT_CAL reply payload. Two implementations exist:
    `KilnctrlCtCalTransport` (the real kilnctrl UartLink) and, in this
    package's tests, an in-memory fake standing in for SaftyFW's own
    config_store read-modify-write semantics -- see this package's test
    file for exactly what it does and does not model."""

    def send_set_ct_cal(self, payload: bytes) -> None: ...

    def query_ct_cal(self, payload: bytes, timeout: float) -> bytes: ...

    def close(self) -> None: ...


class KilnctrlCtCalTransport:
    """Real transport: task UART_TASK_ID_SAFETY (7) on the existing kilnctrl
    UartLink -- the same physical link `readback.py`'s
    `KilnctrlSafetyReadback`/`SafetyClient` use for GET_STATUS, but driven
    here with raw frames instead of through `SafetyClient`, because
    `SafetyClient`'s own receive loop only understands
    GET_STATUS/GET_LINK_STATS replies (`kilnctrl.devices.parse_safety_response`)
    and would silently drop a CT_CAL reply as an unrecognised response --
    not something this module can change, since `tools/PcTools/src/kilnctrl/**`
    is off-limits for this task.

    This means one instance of this class needs EXCLUSIVE use of task 7 on
    the link for its lifetime: it must not run at the same time as a
    `SafetyClient` (or another instance of this class) on the same physical
    link, because `UartLink.register_task()` refuses a duplicate
    registration outright. In this tool's normal usage that is automatic --
    `calibrate_ct.py`'s bench run closes its own `SafetyClient` before
    exiting (`dut.close()` in its `finally` block), and this module's push
    step runs afterward, as a separate command -- but this class does not
    enforce or detect a violation itself; it will simply raise whatever
    `UartLink.register_task()` raises (`ValueError`).

    **See this module's top-of-file docstring for the currently-unresolved
    KilnFW gap that means a real ESP will ACK a SET_CT_CAL and silently
    discard it, and GET_CT_CAL will time out, until `uart_bridge.c` gains
    the missing switch cases.**
    """

    def __init__(self, link, task_id: int = 7) -> None:
        self._link = link
        self._task_id = task_id
        self._inbox = link.register_task(task_id)

    def send_set_ct_cal(self, payload: bytes) -> None:
        from kilnctrl.serial_link import SendResult  # deferred: see readback.py's
        # KilnctrlSafetyReadback for why this module avoids a hard,
        # module-scope dependency on kilnctrl being importable.

        result = self._link.send(dst_task=self._task_id, src_task=self._task_id, payload=payload)
        if result is not SendResult.OK:
            raise CtCalLinkError(f"SET_CT_CAL not delivered: {result.describe()}")

    def query_ct_cal(self, payload: bytes, timeout: float) -> bytes:
        from kilnctrl.serial_link import SendResult

        result = self._link.send(dst_task=self._task_id, src_task=self._task_id, payload=payload)
        if result is not SendResult.OK:
            raise CtCalLinkError(f"GET_CT_CAL not delivered: {result.describe()}")

        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CtCalLinkError(
                    f"GET_CT_CAL was ACKed but no reply arrived within {timeout:.1f}s -- "
                    "if this is a real ESP, see this module's docstring: KilnFW's "
                    "uart_bridge.c does not yet relay this command to the Pico, so "
                    "this is the expected outcome against real hardware today, not "
                    "necessarily a link failure"
                )
            try:
                frame = self._inbox.get(timeout=remaining)
            except queue.Empty:
                continue
            if len(frame.payload) == CT_CAL_LEN and frame.payload[0] == CT_CAL_REPLY_CMD:
                return frame.payload
            # Anything else arriving on task 7 while we wait (e.g. a stale
            # frame) is unexpected but not fatal on its own -- keep waiting
            # up to the deadline for our specific reply shape.

    def close(self) -> None:
        self._link.unregister_task(self._task_id)


# --- Push + verify ------------------------------------------------------------

_GAIN_TOL = 1e-4
_OFFSET_TOL = 1e-4


@dataclass(frozen=True)
class ChannelVerifyResult:
    channel: int
    sent: ChannelPush
    readback: ChannelPush
    matches: bool
    detail: str


class VerifyMismatchError(RuntimeError):
    """Raised by `push_and_verify()` when the post-push GET_CT_CAL readback
    does not match what was sent, for one or more channels. A push that
    isn't read back and confirmed isn't trusted -- these constants sit in
    front of a safety-relevant presence threshold (S3/S4/S9 read
    `current_a`), so this is deliberately an exception, not a warning a
    caller could ignore."""

    def __init__(self, results: "tuple[ChannelVerifyResult, ...]") -> None:
        bad = [r for r in results if not r.matches]
        super().__init__(
            "CT calibration readback did not match what was pushed: " + "; ".join(r.detail for r in bad)
        )
        self.results = results


def _channel_matches(sent: ChannelPush, got: ChannelPush) -> "tuple[bool, str]":
    if sent.calibrated != got.calibrated:
        return False, (
            f"channel {sent.channel}: calibrated flag mismatch "
            f"(sent {sent.calibrated}, read back {got.calibrated})"
        )
    if not sent.calibrated:
        # gain/offset are documented as meaningless once calibrated=False --
        # only the flag itself is checked, never the (ignored) numbers.
        return True, f"channel {sent.channel}: uncalibrated, confirmed"
    if abs(sent.gain - got.gain) > _GAIN_TOL or abs(sent.offset - got.offset) > _OFFSET_TOL:
        return False, (
            f"channel {sent.channel}: gain/offset mismatch "
            f"(sent gain={sent.gain!r} offset={sent.offset!r}, "
            f"read back gain={got.gain!r} offset={got.offset!r})"
        )
    return True, f"channel {sent.channel}: gain={got.gain:.6g} offset={got.offset:.6g}, confirmed"


def push_and_verify(
    transport: CtCalTransport,
    pushes: "Sequence[ChannelPush]",
    timeout: float = 2.0,
    log: Callable[[str], None] = lambda line="": None,
) -> "tuple[ChannelVerifyResult, ...]":
    """Sends one SET_CT_CAL frame per channel (fire-and-forget on the wire --
    see this module's docstring), then issues one GET_CT_CAL query and
    compares every channel's readback against what was just sent. Raises
    `VerifyMismatchError` if any channel disagrees. This is the "verify by
    reading back" step this task requires -- a push that is never read back
    is not confirmed, per this package's task instructions."""
    for push in pushes:
        state = f"calibrated gain={push.gain!r} offset={push.offset!r}" if push.calibrated else "UNCALIBRATED"
        log(f"SET_CT_CAL channel {push.channel}: {state}" + (f"  ({push.provenance})" if push.provenance else ""))
        transport.send_set_ct_cal(encode_set_ct_cal(push))

    log("GET_CT_CAL: reading back what SaftyFW's config_store now holds...")
    reply = transport.query_ct_cal(encode_get_ct_cal(), timeout=timeout)
    readback = decode_ct_cal_reply(reply)

    results = []
    for sent, got in zip(pushes, readback):
        ok, detail = _channel_matches(sent, got)
        log(("  OK: " if ok else "  MISMATCH: ") + detail)
        results.append(ChannelVerifyResult(channel=sent.channel, sent=sent, readback=got, matches=ok, detail=detail))

    if not all(r.matches for r in results):
        raise VerifyMismatchError(tuple(results))
    return tuple(results)


# --- CLI ----------------------------------------------------------------------


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--json", type=Path, required=True, help="calibration table written by calibrate_ct.py")
    p.add_argument(
        "--dut-serial-port", default=None,
        help="serial port for the kilnctrl link to the ESP/Pico pair (omit to use kilnctrl's autodetect)",
    )
    p.add_argument("--timeout", type=float, default=2.0, help="GET_CT_CAL reply timeout, seconds")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)

    def log(line: str = "") -> None:
        print(line)

    log("=== SaftyFW CT calibration push (SET_CT_CAL / GET_CT_CAL) ===")
    try:
        pushes = build_pushes_from_json(args.json)
    except TableError as exc:
        log(f"REFUSED: {exc}")
        return 1

    for p in pushes:
        state = f"calibrated gain={p.gain!r} offset={p.offset!r}" if p.calibrated else "UNCALIBRATED"
        log(f"  channel {p.channel}: {state}" + (f"  ({p.provenance})" if p.provenance else ""))

    if str(_PCTOOLS_SRC) not in sys.path:
        sys.path.insert(0, str(_PCTOOLS_SRC))
    from kilnctrl.link_hub import get_shared_link  # noqa: E402

    link = get_shared_link()
    try:
        used = link.connect(args.dut_serial_port)
    except Exception as exc:  # noqa: BLE001
        log(f"ERROR: could not connect to kilnctrl link: {exc}")
        return 3
    log(f"kilnctrl link connected: {used}")

    transport = KilnctrlCtCalTransport(link)
    try:
        push_and_verify(transport, pushes, timeout=args.timeout, log=log)
    except CtCalLinkError as exc:
        log(f"ERROR: {exc}")
        return 5
    except VerifyMismatchError as exc:
        log(f"VERIFY MISMATCH: {exc}")
        return 6
    finally:
        transport.close()

    log("")
    log("All channels pushed and verified against SaftyFW's own readback.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
