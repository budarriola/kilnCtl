"""Shared CRC-16/CCITT-FALSE implementation for pc_tools' pure-Python link
codecs.

firmware/CommonFW's kilnlink component (kilnlink_crc.c) is the canonical
implementation used by the real link between the ESP32-S3 and the RP2040
safety processor. pc_tools cannot link that C library, so
``kilnctrl.protocol`` carries a documented pure-Python port of it (see its
own docstring, "Bit-for-bit port of crc16_ccitt_false() in
uart_protocol.c") -- that port is the allowlisted, justified duplicate
tracked in tools/check_no_duplicate_crc.ps1.

kilnsim's benchproto (the bench-fixture <-> PC protocol, a distinct wire
format from kilnlink) needs the exact same CRC-16/CCITT-FALSE algorithm.
Unlike the kilnlink port, there is no cross-language linking barrier here:
both kilnctrl.protocol and kilnsim.benchproto_codec are pure Python living
side by side under tools/PcTools/src, and kilnsim already imports from
kilnctrl freely (see e.g. kilnsim.guard_observer, kilnsim.testmgr). A
second, independently-typed copy of a CRC is exactly the kind of thing that
silently diverges, so this module exists to give both codecs one
implementation instead of two.

Check value: crc16_ccitt_false(b"123456789") == 0x29B1.
"""

from __future__ import annotations

__all__ = ["crc16_ccitt_false"]


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.

    Bit-for-bit port of ``crc16_ccitt_false()`` in uart_protocol.c (by way of
    firmware/CommonFW's kilnlink_crc.c, the canonical C implementation).
    Pure Python on purpose -- no external crc dependency.
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF
