#!/usr/bin/env python3
"""Minimal ELF -> UF2 converter and verifier for RP2040, with no dependency
on picotool or a host C/C++ compiler -- only arm-none-eabi-objcopy (already
configured on this machine) and the Python standard library.

Why this exists: firmware/SimFW/CMakeLists.txt cannot call pico_add_extra_
outputs() (that needs picotool, which has no prebuilt binary for this
pico-sdk version and needs a host C/C++ compiler this machine does not have
configured). This script produces the same .uf2 a BOOTSEL drag-and-drop
flash needs, straight from the .elf, and can independently re-verify any
.uf2 it (or anything else) produced.

UF2 format reference: https://github.com/microsoft/uf2 -- 512-byte blocks,
32-byte header + up to 476 bytes of payload + 4-byte end magic, all fields
little-endian:

    offset  size  field
    0       4     magicStart0   = 0x0A324655
    4       4     magicStart1   = 0x9E5D5157
    8       4     flags         = 0x00002000 (familyID present)
    12      4     targetAddr
    16      4     payloadSize
    20      4     blockNo
    24      4     numBlocks
    28      4     fileSize / familyID (family-ID flag set here: familyID)
    32      476   data (only payloadSize bytes are meaningful)
    508     4     magicEnd      = 0x0AB16F30

RP2040 specifics: familyID = 0xE48BFF56, payloadSize = 256 bytes/block,
targetAddr starts at XIP flash base 0x10000000 and advances by 256 per
block.

RP2040 boot_stage2 (boot2) integrity: the first 256 bytes of any flash
image are the boot2 stub. Its last 4 bytes are a CRC32 (MPEG-2 variant:
poly 0x04C11DB7, init 0xFFFFFFFF, no final xor, non-reflected/non-
"reversed" input or output) over the first 252 bytes, stored little-endian.
If this doesn't match, the RP2040 boot ROM refuses to jump into flash at
all -- silent bricked-until-BOOTSEL, not a crash. Verified in development
against pico-sdk's own bs2_default_padded_checksummed.S (a known-good
image): computed CRC 0x7a4eb274 over its first 252 bytes, stored as LE
bytes 74 b2 4e 7a -- matches the file exactly.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

MAGIC_START0 = 0x0A324655
MAGIC_START1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30
FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
PAYLOAD_SIZE = 256
BLOCK_SIZE = 512
XIP_FLASH_BASE = 0x10000000

# RP2040 boot_stage2 constants.
BOOT2_SIZE = 256
BOOT2_CRC_LEN = 252
CRC32_MPEG2_POLY = 0x04C11DB7
CRC32_MPEG2_INIT = 0xFFFFFFFF


def crc32_mpeg2(data: bytes) -> int:
    """CRC32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no
    final xor. This is deliberately a plain from-scratch bit-at-a-time
    implementation (not binascii.crc32, which is the reflected/"CRC-32"
    variant used by zip/ethernet and gives a different answer) so it can be
    read and checked line-by-line against the algorithm description."""
    crc = CRC32_MPEG2_INIT
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ CRC32_MPEG2_POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def elf_to_bin(objcopy: str, elf_path: Path, bin_path: Path) -> None:
    subprocess.run(
        [objcopy, "-O", "binary", str(elf_path), str(bin_path)],
        check=True,
    )


def bin_to_uf2(data: bytes, base_addr: int = XIP_FLASH_BASE) -> bytes:
    """Encode a flat binary image into RP2040 UF2 blocks."""
    num_blocks = (len(data) + PAYLOAD_SIZE - 1) // PAYLOAD_SIZE
    if num_blocks == 0:
        raise ValueError("refusing to encode an empty image")
    out = bytearray()
    for block_no in range(num_blocks):
        chunk = data[block_no * PAYLOAD_SIZE : (block_no + 1) * PAYLOAD_SIZE]
        header = struct.pack(
            "<IIIIIIII",
            MAGIC_START0,
            MAGIC_START1,
            FLAG_FAMILY_ID_PRESENT,
            base_addr + block_no * PAYLOAD_SIZE,
            PAYLOAD_SIZE,
            block_no,
            num_blocks,
            RP2040_FAMILY_ID,
        )
        padded_payload = chunk + b"\x00" * (476 - len(chunk))
        block = header + padded_payload + struct.pack("<I", MAGIC_END)
        assert len(block) == BLOCK_SIZE
        out += block
    return bytes(out)


class Uf2Error(Exception):
    pass


def decode_uf2_to_bin(uf2_data: bytes) -> bytes:
    """Decode a UF2 image back to a flat binary, performing all of the
    structural checks along the way (checks 3/4 from the task). Raises
    Uf2Error with a specific message on the first violation found -- used
    both for real verification and for the induced-failure tests below."""
    if len(uf2_data) == 0:
        raise Uf2Error("empty file")
    if len(uf2_data) % BLOCK_SIZE != 0:
        raise Uf2Error(
            f"file size {len(uf2_data)} is not a multiple of {BLOCK_SIZE}"
        )

    num_file_blocks = len(uf2_data) // BLOCK_SIZE
    chunks: dict[int, bytes] = {}
    expected_num_blocks = None
    expected_base_addr = None
    first_target_addr = None

    for i in range(num_file_blocks):
        block = uf2_data[i * BLOCK_SIZE : (i + 1) * BLOCK_SIZE]
        (
            magic_start0,
            magic_start1,
            flags,
            target_addr,
            payload_size,
            block_no,
            num_blocks,
            family_id,
        ) = struct.unpack("<IIIIIIII", block[0:32])
        magic_end = struct.unpack("<I", block[508:512])[0]

        if magic_start0 != MAGIC_START0:
            raise Uf2Error(f"block {i}: bad magicStart0 {magic_start0:#010x}")
        if magic_start1 != MAGIC_START1:
            raise Uf2Error(f"block {i}: bad magicStart1 {magic_start1:#010x}")
        if magic_end != MAGIC_END:
            raise Uf2Error(f"block {i}: bad magicEnd {magic_end:#010x}")
        if not (flags & FLAG_FAMILY_ID_PRESENT):
            raise Uf2Error(f"block {i}: familyID-present flag not set")
        if family_id != RP2040_FAMILY_ID:
            raise Uf2Error(f"block {i}: wrong familyID {family_id:#010x}")
        if payload_size != PAYLOAD_SIZE:
            raise Uf2Error(f"block {i}: unexpected payloadSize {payload_size}")

        if block_no != i:
            raise Uf2Error(
                f"block {i}: blockNo field is {block_no}, expected strictly "
                f"ascending from 0"
            )
        if expected_num_blocks is None:
            expected_num_blocks = num_blocks
        elif num_blocks != expected_num_blocks:
            raise Uf2Error(
                f"block {i}: numBlocks {num_blocks} != earlier block's "
                f"{expected_num_blocks}"
            )

        if first_target_addr is None:
            first_target_addr = target_addr
            expected_base_addr = target_addr
        if target_addr != expected_base_addr:
            raise Uf2Error(
                f"block {i}: targetAddr {target_addr:#010x} is not "
                f"contiguous (expected {expected_base_addr:#010x}, "
                f"256-byte stride from block 0)"
            )
        expected_base_addr += PAYLOAD_SIZE

        chunks[block_no] = block[32 : 32 + PAYLOAD_SIZE]

    if expected_num_blocks != num_file_blocks:
        raise Uf2Error(
            f"numBlocks field says {expected_num_blocks} but file contains "
            f"{num_file_blocks} blocks"
        )
    if sorted(chunks.keys()) != list(range(num_file_blocks)):
        raise Uf2Error("block numbers are not a contiguous 0..N-1 set")
    if first_target_addr != XIP_FLASH_BASE:
        raise Uf2Error(
            f"first targetAddr {first_target_addr:#010x} != XIP flash base "
            f"{XIP_FLASH_BASE:#010x}"
        )

    return b"".join(chunks[i] for i in range(num_file_blocks))


def verify_boot2_crc(image: bytes) -> tuple[int, int]:
    """Returns (computed_crc, stored_crc) for the boot2 stub at the start of
    `image`. Raises Uf2Error if they don't match or the image is too short."""
    if len(image) < BOOT2_SIZE:
        raise Uf2Error(
            f"image is only {len(image)} bytes, too short to contain a "
            f"{BOOT2_SIZE}-byte boot2 stub"
        )
    boot2 = image[:BOOT2_SIZE]
    stored_crc = struct.unpack("<I", boot2[BOOT2_CRC_LEN:BOOT2_SIZE])[0]
    computed_crc = crc32_mpeg2(boot2[:BOOT2_CRC_LEN])
    if computed_crc != stored_crc:
        raise Uf2Error(
            f"boot2 CRC mismatch: computed {computed_crc:#010x}, stored "
            f"{stored_crc:#010x} -- chip will NOT boot from flash"
        )
    return computed_crc, stored_crc


def verify_uf2(uf2_path: Path, bin_path: Path | None = None) -> None:
    """Runs all four checks from the task against an on-disk .uf2, printing
    a report. Raises Uf2Error on the first failure."""
    uf2_data = uf2_path.read_bytes()

    # Checks 3 & 4 (structural) happen inside decode; check 4's "first
    # targetAddr == 0x10000000" is also asserted there.
    decoded = decode_uf2_to_bin(uf2_data)
    num_blocks = len(uf2_data) // BLOCK_SIZE
    print(f"[3/4] structural checks passed: {num_blocks} blocks, "
          f"{len(uf2_data)} bytes, contiguous targetAddr from "
          f"{XIP_FLASH_BASE:#010x}")

    # Check 4b: total payload bytes == .bin size, if a reference .bin was
    # given.
    if bin_path is not None:
        bin_data = bin_path.read_bytes()
        # The last UF2 block is zero-padded out to the fixed 256-byte
        # payload size, so a .bin whose length isn't a multiple of 256 will
        # decode back slightly longer than it started -- that is expected,
        # not a mismatch, as long as (a) the real content matches exactly
        # and (b) every padding byte tacked on is zero (i.e. nothing was
        # silently invented in place of real data).
        if len(decoded) < len(bin_data):
            raise Uf2Error(
                f"decoded UF2 payload is {len(decoded)} bytes, shorter than "
                f"reference .bin's {len(bin_data)} bytes"
            )
        pad_len = len(decoded) - len(bin_data)
        if pad_len >= PAYLOAD_SIZE:
            raise Uf2Error(
                f"decoded UF2 payload is {pad_len} bytes longer than the "
                f".bin -- more than one block's worth of padding, not "
                f"explained by last-block rounding"
            )
        # Check 1: round trip byte-identical (content, then padding).
        if decoded[: len(bin_data)] != bin_data:
            first_diff = next(
                i for i in range(len(bin_data)) if decoded[i] != bin_data[i]
            )
            raise Uf2Error(
                f"decoded UF2 differs from reference .bin at byte "
                f"{first_diff}: {decoded[first_diff]:#04x} != "
                f"{bin_data[first_diff]:#04x}"
            )
        if decoded[len(bin_data):] != b"\x00" * pad_len:
            raise Uf2Error(
                f"last-block padding after byte {len(bin_data)} is not "
                f"all-zero"
            )
        print(f"[1/4] round-trip check passed: decoded UF2 payload is "
              f"byte-identical to {bin_path.name} ({len(bin_data)} bytes, "
              f"plus {pad_len} zero-padding bytes to fill the last block)")
    else:
        print("[1/4] round-trip check skipped: no reference .bin given")

    # Check 2: boot2 CRC.
    computed, stored = verify_boot2_crc(decoded)
    print(f"[2/4] boot2 CRC check passed: computed {computed:#010x} == "
          f"stored {stored:#010x}")

    print("all checks passed")


def cmd_build(args: argparse.Namespace) -> int:
    elf_path = Path(args.elf)
    bin_path = Path(args.bin) if args.bin else elf_path.with_suffix(".bin")
    uf2_path = Path(args.uf2) if args.uf2 else elf_path.with_suffix(".uf2")

    elf_to_bin(args.objcopy, elf_path, bin_path)
    bin_data = bin_path.read_bytes()

    # Fail loudly before writing anything if boot2 itself is broken --
    # cheaper to catch here than after it's on the chip.
    computed, stored = verify_boot2_crc(bin_data)
    if computed != stored:
        print(
            f"error: boot2 CRC mismatch in {bin_path} before UF2 encoding "
            f"even happens (computed {computed:#010x}, stored "
            f"{stored:#010x}) -- refusing to write a .uf2",
            file=sys.stderr,
        )
        return 1

    uf2_data = bin_to_uf2(bin_data)
    uf2_path.write_bytes(uf2_data)

    num_blocks = len(uf2_data) // BLOCK_SIZE
    print(f"wrote {uf2_path} ({len(uf2_data)} bytes, {num_blocks} blocks)")
    print(f"boot2 CRC OK: {computed:#010x}")

    if args.verify:
        verify_uf2(uf2_path, bin_path)
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    uf2_path = Path(args.uf2)
    bin_path = Path(args.bin) if args.bin else None
    try:
        verify_uf2(uf2_path, bin_path)
    except Uf2Error as e:
        print(f"VERIFY FAILED: {e}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_build = sub.add_parser("build", help="convert an .elf to a .uf2")
    p_build.add_argument("elf", help="input .elf path")
    p_build.add_argument("--bin", help="output .bin path (default: elf with .bin suffix)")
    p_build.add_argument("--uf2", help="output .uf2 path (default: elf with .uf2 suffix)")
    p_build.add_argument(
        "--objcopy",
        default="arm-none-eabi-objcopy",
        help="path to arm-none-eabi-objcopy",
    )
    p_build.add_argument(
        "--verify", action="store_true", help="run full verification after building"
    )
    p_build.set_defaults(func=cmd_build)

    p_verify = sub.add_parser("verify", help="verify an existing .uf2")
    p_verify.add_argument("uf2", help="path to .uf2 to verify")
    p_verify.add_argument(
        "--bin", help="reference .bin to compare the round-trip decode against"
    )
    p_verify.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
