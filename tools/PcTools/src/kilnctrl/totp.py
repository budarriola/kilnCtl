#!/usr/bin/env python3
"""totp.py -- pure-Python RFC 6238 TOTP helper.

PC-side only: nothing here talks to a board, a file, or NVS. It exists so
WT-C's ``totp_reset_password()`` MCP tool never has to hand a raw shared
secret anywhere -- in this design the PC side never even HOLDS the board's
TOTP secret; it only reads a 6-digit CODE the operator already generated on
their own authenticator app, from ``KILNCTL_TOTP_CODE``, and forwards it
verbatim to the board's ``POST /api/auth/forgot`` route. This module is not
called anywhere in that path today.

It is provided because WT-D (firmware host tests, not yet written) needs
the exact same HMAC-SHA1/counter/truncation math the ESP side will
implement, and RFC 6238 Appendix B's published test vectors are the only
independent way to confirm a from-scratch implementation is correct before
a firmware implementation exists to compare against. Keeping one verified
implementation on the PC side that a future firmware host test can be
checked against by hand (same seed, same counter, same expected digits) is
cheaper than each side deriving the RFC's HMAC/counter/truncation steps
independently and hoping they agree.

RFC 6238 fixes HOTP (RFC 4226) to a counter derived from wall-clock time:

    C = floor(unix_time / period)

and HOTP itself is: HMAC(secret, counter-as-8-byte-big-endian) -> dynamic
truncation (RFC 4226 section 5.3) -> mod 10**digits, zero-padded.

This module hardcodes the parameters this project's plan
(docs/TOTP_PASSWORD_RESET_PLAN.md section 1) requires to be fixed, not
configurable: HMAC-SHA1, 6 digits, 30 second period. RFC 6238 Appendix B's
own test vectors are stated as 8-digit; :func:`hotp` accepts a ``digits``
parameter (used only by the test vectors below) so this module can be
checked against those published values directly, while :func:`totp` -- the
function any real caller uses -- always produces the fixed 6-digit code.
"""
from __future__ import annotations

import hashlib
import hmac
import struct
import time
from typing import Optional

#: Fixed per docs/TOTP_PASSWORD_RESET_PLAN.md section 1 -- not configurable.
#: At least three of the eight target authenticator apps are documented to
#: ignore or mishandle a non-default algorithm/digit-count/period, so no
#: caller in this codebase should ever pass a different value.
DEFAULT_DIGITS = 6
DEFAULT_PERIOD_S = 30
DEFAULT_ALGORITHM = "sha1"

_ALGORITHMS = {
    "sha1": hashlib.sha1,
    "sha256": hashlib.sha256,
    "sha512": hashlib.sha512,
}

#: RFC 4648 base32 alphabet, uppercase, no padding on encode.
_BASE32_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"


def hotp(secret: bytes, counter: int, digits: int = DEFAULT_DIGITS,
         algorithm: str = DEFAULT_ALGORITHM) -> str:
    """RFC 4226 HOTP: HMAC(secret, counter) -> dynamic truncation -> N digits.

    ``counter`` is encoded as an 8-byte big-endian integer per the RFC.
    Dynamic truncation (RFC 4226 section 5.3): take the low nibble of the
    last HMAC byte as an offset, read 4 bytes from that offset as a
    big-endian unsigned 31-bit integer (top bit masked off), then reduce
    mod ``10**digits`` and zero-pad to ``digits`` characters.
    """
    if digits <= 0:
        raise ValueError("digits must be positive")
    try:
        digestmod = _ALGORITHMS[algorithm.lower()]
    except KeyError as exc:
        raise ValueError(f"unsupported algorithm: {algorithm!r}") from exc

    counter_bytes = struct.pack(">Q", counter)
    mac = hmac.new(secret, counter_bytes, digestmod).digest()

    offset = mac[-1] & 0x0F
    truncated = struct.unpack(">I", mac[offset:offset + 4])[0] & 0x7FFFFFFF
    code = truncated % (10 ** digits)
    return str(code).zfill(digits)


def totp(secret: bytes, at_time: Optional[float] = None, period: int = DEFAULT_PERIOD_S,
          digits: int = DEFAULT_DIGITS, algorithm: str = DEFAULT_ALGORITHM) -> str:
    """RFC 6238 TOTP: :func:`hotp` at counter ``floor(at_time / period)``.

    ``at_time`` defaults to ``time.time()``. Always produces a fixed-width
    ``digits``-character code (default 6, per this project's plan)."""
    if at_time is None:
        at_time = time.time()
    counter = int(at_time // period)
    return hotp(secret, counter, digits=digits, algorithm=algorithm)


def verify_totp(secret: bytes, code: str, at_time: Optional[float] = None,
                 period: int = DEFAULT_PERIOD_S, digits: int = DEFAULT_DIGITS,
                 algorithm: str = DEFAULT_ALGORITHM, window: int = 1) -> bool:
    """True if ``code`` matches the TOTP code at ``at_time``'s counter step,
    or any of the ``window`` steps immediately before/after it (the
    standard RFC 6238 clock-skew allowance -- the plan's section 3 specifies
    +/-1 step). Comparison is constant-time (``hmac.compare_digest``) so a
    timing side channel cannot leak how many leading digits matched --
    matching this project's existing discipline for secret-derived
    comparisons (CLAUDE.md's TOTP plan section 3)."""
    if at_time is None:
        at_time = time.time()
    counter = int(at_time // period)
    code = code.strip()
    for offset in range(-window, window + 1):
        candidate_counter = counter + offset
        if candidate_counter < 0:
            # Only reachable within `window` steps of unix time 0 -- HOTP's
            # counter is an unsigned 64-bit quantity (RFC 4226), so a
            # negative candidate is never valid and is skipped rather than
            # raising out of a verification call.
            continue
        candidate = hotp(secret, candidate_counter, digits=digits, algorithm=algorithm)
        if hmac.compare_digest(candidate, code):
            return True
    return False


def base32_encode(data: bytes) -> str:
    """RFC 4648 base32, uppercase, no padding -- the shape this project's
    plan requires for both the manual-entry key and the otpauth URI's
    ``secret=`` field."""
    bits = 0
    value = 0
    out = []
    for byte in data:
        value = (value << 8) | byte
        bits += 8
        while bits >= 5:
            bits -= 5
            out.append(_BASE32_ALPHABET[(value >> bits) & 0x1F])
    if bits > 0:
        out.append(_BASE32_ALPHABET[(value << (5 - bits)) & 0x1F])
    return "".join(out)


def base32_decode(text: str) -> bytes:
    """Inverse of :func:`base32_encode`. Accepts (and ignores) trailing
    ``=`` padding and is case-insensitive, since a human may retype a
    manual-entry key in either case or with padding an app added."""
    text = text.strip().rstrip("=").upper()
    bits = 0
    value = 0
    out = bytearray()
    for ch in text:
        try:
            idx = _BASE32_ALPHABET.index(ch)
        except ValueError as exc:
            raise ValueError(f"invalid base32 character: {ch!r}") from exc
        value = (value << 5) | idx
        bits += 5
        if bits >= 8:
            bits -= 8
            out.append((value >> bits) & 0xFF)
    return bytes(out)
