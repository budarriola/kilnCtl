#!/usr/bin/env python3
"""esp_app_desc.py -- parses the ``esp_app_desc_t`` app-description struct
that ESP-IDF embeds in every app image, to read back the build date/time the
running firmware itself will later report over ``GET /api/status``
(``fw_build``, e.g. ``"Sep  3 2026 20:13:41"``).

Added alongside flash_firmware()'s post-flash verification (see
mcp_server_flash.py) after a recurring failure mode: flash_firmware() writes
only the ``factory`` partition and never touches ``otadata``. If an OTA ever
pointed the boot target at ``ota_0``/``ota_1``, the bootloader keeps booting
that OLD image forever -- every later flash_firmware() reports "flashed and
verified OK" (which is only OpenOCD's own byte-compare during the write) while
the board silently keeps running the old code. The fix needs an independent,
after-the-fact confirmation that the binary just flashed is the one actually
running -- comparing the RUNNING partition (see partition_http_client.py) is
necessary but not sufficient (a stale rebuild of ``factory`` itself would
still look right); comparing build timestamps closes that gap.

Deliberately does NOT trust the .bin file's mtime for this -- mtime survives
a `git checkout`/copy/clone untouched and says nothing about what commit or
edit actually produced the bytes. The build timestamp burned into the image
itself by the ESP-IDF build (``__DATE__``/``__TIME__`` at compile time) is the
only signal that travels with the bytes, so this module parses it directly
out of the image rather than out of the filesystem.

Image layout (see esp-idf's esp_app_format.h):

    offset 0x00: esp_image_header_t                (24 bytes / 0x18)
    offset 0x18: esp_image_segment_header_t         (8 bytes / 0x08)
    offset 0x20: esp_app_desc_t                     (the struct this module reads)

``esp_app_desc_t`` (fixed field widths, little-endian):

    magic_word      uint32   4   -- must be ESP_APP_DESC_MAGIC_WORD (0xABCD5432)
    secure_version  uint32   4
    reserv1         uint32*2 8
    version         char[32] 32
    project_name    char[32] 32
    time            char[16] 16  -- e.g. "20:13:41"
    date            char[16] 16  -- e.g. "Sep  3 2026"
    idf_ver         char[32] 32
    app_elf_sha256  uint8[32]32
    reserv2         uint32*20 80

Only magic_word/time/date are read here -- nothing else is needed for the
verification this module exists for.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

#: esp_app_format.h ESP_APP_DESC_MAGIC_WORD
ESP_APP_DESC_MAGIC_WORD = 0xABCD5432

#: esp_app_desc_t starts right after the 24-byte image header + 8-byte first
#: segment header.
APP_DESC_OFFSET = 0x20

_MAGIC_FMT = "<I"  # uint32, little-endian
_MAGIC_SIZE = 4
_SECURE_VERSION_SIZE = 4
_RESERV1_SIZE = 8
_VERSION_SIZE = 32
_PROJECT_NAME_SIZE = 32
_TIME_SIZE = 16
_DATE_SIZE = 16

# Offsets relative to APP_DESC_OFFSET.
_OFF_SECURE_VERSION = _MAGIC_SIZE
_OFF_RESERV1 = _OFF_SECURE_VERSION + _SECURE_VERSION_SIZE
_OFF_VERSION = _OFF_RESERV1 + _RESERV1_SIZE
_OFF_PROJECT_NAME = _OFF_VERSION + _VERSION_SIZE
_OFF_TIME = _OFF_PROJECT_NAME + _PROJECT_NAME_SIZE
_OFF_DATE = _OFF_TIME + _TIME_SIZE
_APP_DESC_MIN_SIZE = _OFF_DATE + _DATE_SIZE  # everything up through `date`


class AppDescError(ValueError):
    """Raised when a blob is too short to contain an esp_app_desc_t, or its
    magic word doesn't match -- i.e. it isn't a valid ESP-IDF app image (or
    the offset assumption above doesn't hold for it)."""


@dataclass(frozen=True)
class AppDesc:
    version: str
    project_name: str
    time: str
    date: str

    @property
    def build_timestamp(self) -> str:
        """``"<date> <time>"`` in the same shape the board's own fw_build
        status field uses (e.g. "Sep  3 2026 20:13:41"), so the two can be
        string-compared directly."""
        return f"{self.date} {self.time}"


def _decode_cstr(raw: bytes) -> str:
    """Fixed-width char[] field -> Python str, stopping at the first NUL
    (ESP-IDF pads these fields with NULs after the string)."""
    return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def parse_app_desc(image_bytes: bytes) -> AppDesc:
    """Parses the esp_app_desc_t out of a full app image (e.g. the bytes of
    KilnCtrl.bin). Raises AppDescError with a clear message if the image is
    too short to contain the descriptor at the expected offset, or if the
    magic word doesn't match -- callers should treat either as "could not
    verify" (loud), never silently skip."""
    end = APP_DESC_OFFSET + _APP_DESC_MIN_SIZE
    if len(image_bytes) < end:
        raise AppDescError(
            f"image is only {len(image_bytes)} bytes -- too short to contain an "
            f"esp_app_desc_t at offset 0x{APP_DESC_OFFSET:x} (need at least {end})"
        )

    desc = image_bytes[APP_DESC_OFFSET:end]
    (magic,) = struct.unpack_from(_MAGIC_FMT, desc, 0)
    if magic != ESP_APP_DESC_MAGIC_WORD:
        raise AppDescError(
            f"bad esp_app_desc_t magic word: got 0x{magic:08X}, expected "
            f"0x{ESP_APP_DESC_MAGIC_WORD:08X} -- this doesn't look like a valid "
            "ESP-IDF app image (or the image format has changed)"
        )

    version = _decode_cstr(desc[_OFF_VERSION:_OFF_VERSION + _VERSION_SIZE])
    project_name = _decode_cstr(desc[_OFF_PROJECT_NAME:_OFF_PROJECT_NAME + _PROJECT_NAME_SIZE])
    time_s = _decode_cstr(desc[_OFF_TIME:_OFF_TIME + _TIME_SIZE])
    date_s = _decode_cstr(desc[_OFF_DATE:_OFF_DATE + _DATE_SIZE])
    return AppDesc(version=version, project_name=project_name, time=time_s, date=date_s)


def parse_app_desc_file(bin_path: str) -> AppDesc:
    """Convenience wrapper: reads `bin_path` and parses its esp_app_desc_t.
    Only the first `APP_DESC_OFFSET + _APP_DESC_MIN_SIZE` bytes are actually
    needed, but app images are small enough (~1-2MB) that reading the whole
    file is simpler and not worth optimizing."""
    with open(bin_path, "rb") as f:
        image_bytes = f.read()
    return parse_app_desc(image_bytes)


def build_timestamps_match(app_desc: AppDesc, fw_build: Optional[str]) -> bool:
    """Compares an AppDesc's build_timestamp against a board-reported
    `fw_build` string (dashboard_http.c's fw_build field, same "Mon D YYYY
    HH:MM:SS" shape __DATE__/__TIME__ produce). Whitespace-normalized on both
    sides -- ESP-IDF's __DATE__ pads single-digit days with an extra space
    ("Sep  3 2026") which is easy to introduce or lose when a value passes
    through JSON/logging, and that difference is not a meaningful mismatch."""
    if fw_build is None:
        return False
    return " ".join(app_desc.build_timestamp.split()) == " ".join(fw_build.split())
