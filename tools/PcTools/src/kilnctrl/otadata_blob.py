"""Builds the two ESP-IDF otadata sectors that select the `app` (ota_0) slot.

Layout and CRC come from ESP-IDF itself (checked against v6.0.2):
  components/bootloader_support/include/esp_flash_partitions.h
      esp_ota_select_entry_t { u32 ota_seq; u8 seq_label[20]; u32 ota_state; u32 crc; }  (32 B)
  components/bootloader_support/src/bootloader_common_loader.c
      crc = esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4)
  components/app_update/otatool.py
      binascii.crc32(struct.pack('I', seq), 0xFFFFFFFF)
The bootloader picks slot (ota_seq - 1) % app_count, so ota_seq=1 selects ota_0.
Sector 0 and sector 1 (at +erase_size 0x1000) each carry one entry; both are
written identically here so either copy is valid.
"""
from __future__ import annotations

import binascii
import struct

ENTRY_SIZE = 32
SECTOR_SIZE = 0x1000
OTA_STATE_UNDEFINED = 0xFFFFFFFF


def ota_select_crc(ota_seq: int) -> int:
    return binascii.crc32(struct.pack("<I", ota_seq), 0xFFFFFFFF) & 0xFFFFFFFF


def build_entry(ota_seq: int = 1) -> bytes:
    if not (1 <= ota_seq < 0xFFFFFFFF):
        raise ValueError("ota_seq must be 1..0xFFFFFFFE")
    entry = struct.pack("<I20sII", ota_seq, b"\xff" * 20, OTA_STATE_UNDEFINED, ota_select_crc(ota_seq))
    assert len(entry) == ENTRY_SIZE
    return entry


def build_otadata_image(size: int, ota_seq: int = 1) -> bytes:
    """Whole otadata partition image: entry at 0 and at SECTOR_SIZE, 0xFF elsewhere."""
    if size < 2 * SECTOR_SIZE:
        raise ValueError(f"otadata partition too small ({size} B)")
    img = bytearray(b"\xff" * size)
    entry = build_entry(ota_seq)
    img[0:ENTRY_SIZE] = entry
    img[SECTOR_SIZE:SECTOR_SIZE + ENTRY_SIZE] = entry
    return bytes(img)
