"""Shared helpers for the build_kilnfw tests: write the artifacts a real
``idf.py build`` leaves behind so the real artifact gate runs (not a bypass)."""
from __future__ import annotations

import os
import struct
import time
from typing import Optional

APP_DESC_MAGIC = 0xABCD5432


def make_app_image(built_at: Optional[float] = None, magic: int = APP_DESC_MAGIC) -> bytes:
    """Smallest byte string parse_app_desc() accepts, stamped with ``built_at``
    (local time, like __DATE__/__TIME__)."""
    stamp = time.localtime(time.time() if built_at is None else built_at)
    desc = struct.pack("<I", magic) + b"\0" * 4 + b"\0" * 8
    desc += b"v".ljust(32, b"\0") + b"KilnCtrl".ljust(32, b"\0")
    desc += time.strftime("%H:%M:%S", stamp).encode().ljust(16, b"\0")
    desc += time.strftime("%b %d %Y", stamp).encode().ljust(16, b"\0")
    return b"\0" * 0x20 + desc + b"\0" * 64


def write_app_bin(build_dir: str, built_at: Optional[float] = None, mtime: Optional[float] = None) -> str:
    os.makedirs(build_dir, exist_ok=True)
    path = os.path.join(build_dir, "KilnCtrl.bin")
    with open(path, "wb") as fh:
        fh.write(make_app_image(built_at))
    if mtime is not None:
        os.utime(path, (mtime, mtime))
    return path


def write_static_bins(build_dir: str, age_s: float = 0.0) -> None:
    old = time.time() - age_s
    for parts in (("bootloader", "bootloader.bin"), ("partition_table", "partition-table.bin")):
        p = os.path.join(build_dir, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(b"x")
        os.utime(p, (old, old))


def write_fresh_bins(build_dir: str) -> None:
    """What a successful full or incremental build leaves behind."""
    write_app_bin(build_dir)
    write_static_bins(build_dir)
