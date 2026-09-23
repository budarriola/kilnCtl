#!/usr/bin/env python3
"""check_web_gzip_parity.py -- standing guard for TODO.md 10.6a's open item:
"Re-verify byte-for-byte that nothing about the page content changed" for the
KilnFW web assets that App/drivers/CMakeLists.txt pre-gzips at CMake
configure time.

WHAT THIS CHECKS. `firmware/KilnFW/App/drivers/CMakeLists.txt` gzips a fixed
list of source files (`KILNCTL_GZIP_ASSETS`) into
`${CMAKE_CURRENT_BINARY_DIR}/<basename>.gz` (in practice
`firmware/KilnFW/build/esp-idf/drivers/<basename>.gz`) at *configure* time,
then embeds those .gz files via EMBED_TXTFILES. Nothing previously verified
that an embedded .gz actually decompresses back to byte-identical content of
its own source -- a stale build-dir .gz left over from an older source
revision (CMake only re-gzips on reconfigure, triggered by
CMAKE_CONFIGURE_DEPENDS) or a hand-edited .gz would ship silently. This
script parses the asset list straight out of that CMakeLists.txt (so the two
never drift apart), decompresses each corresponding .gz in the build
directory, and byte-compares it against the source file.

SKIP CONDITION. The .gz files are build output, not tracked in git -- a
fresh checkout or a worktree that never ran the KilnFW target build has none
of them. That is expected, not a defect: this script SKIPs (exit 3, printing
a line containing "SKIP") whenever the build output directory or the
asset list itself cannot be found, rather than failing a checkout that
simply hasn't built yet. Once at least one .gz is missing but others exist
(a partial/stale build tree) that half of the picture is reported as
informational SKIP detail, not a mismatch -- only bytes that both a source
and its embedded .gz agree exist get compared.

Exit codes (matching this repo's check convention, see
tools/check_no_doubled_apostrophes.py and run_all_checks.ps1's header):
  0 -- PASS, at least one asset compared and every compared pair matched.
  1 -- FAIL, at least one asset's decompressed .gz differs from its source.
  3 -- SKIP, no build output / asset list found to check against at all.

Usage: python tools/check_web_gzip_parity.py [--root REPO_ROOT]
"""
from __future__ import annotations

import argparse
import gzip
import re
import sys
from pathlib import Path

CMAKE_REL = Path("firmware/KilnFW/App/drivers/CMakeLists.txt")
DRIVERS_DIR_REL = Path("firmware/KilnFW/App/drivers")
# Where CMAKE_CURRENT_BINARY_DIR actually lands for this ESP-IDF component,
# confirmed against a real build tree (firmware/KilnFW/build/esp-idf/drivers).
BUILD_GZ_DIR_REL = Path("firmware/KilnFW/build/esp-idf/drivers")

ASSET_LIST_RE = re.compile(
    r"set\(KILNCTL_GZIP_ASSETS\s*(.*?)\)", re.DOTALL
)
QUOTED_RE = re.compile(r'"([^"]+)"')


def parse_asset_list(cmake_text: str) -> list[str]:
    m = ASSET_LIST_RE.search(cmake_text)
    if not m:
        return []
    return QUOTED_RE.findall(m.group(1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=None, help="repo root (default: two levels up from this file)"
    )
    args = parser.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[1]

    cmake_path = root / CMAKE_REL
    if not cmake_path.is_file():
        print(f"SKIP: check_web_gzip_parity: {cmake_path} not found")
        return 3

    assets = parse_asset_list(cmake_path.read_text(encoding="utf-8"))
    if not assets:
        print(
            f"SKIP: check_web_gzip_parity: could not parse KILNCTL_GZIP_ASSETS out of {cmake_path}"
        )
        return 3

    build_gz_dir = root / BUILD_GZ_DIR_REL
    if not build_gz_dir.is_dir():
        print(
            f"SKIP: check_web_gzip_parity: {build_gz_dir} does not exist -- "
            "no KilnFW build output to check yet (build the KilnFW target first)"
        )
        return 3

    drivers_dir = root / DRIVERS_DIR_REL
    compared = 0
    missing: list[str] = []
    mismatches: list[str] = []

    for asset in assets:
        src_path = drivers_dir / asset
        gz_path = build_gz_dir / (Path(asset).name + ".gz")
        if not src_path.is_file():
            missing.append(f"{src_path} (source missing)")
            continue
        if not gz_path.is_file():
            missing.append(f"{gz_path} (build output missing)")
            continue
        try:
            with gzip.open(gz_path, "rb") as f:
                decompressed = f.read()
        except Exception as exc:  # noqa: BLE001 -- any decompress failure is a
            # reportable mismatch (corrupt/truncated/hand-edited .gz), not a
            # reason to crash the check with a traceback.
            mismatches.append(f"{gz_path}: failed to decompress ({exc})")
            continue
        src_bytes = src_path.read_bytes()
        compared += 1
        if decompressed != src_bytes:
            mismatches.append(
                f"{gz_path}: decompressed {len(decompressed)} bytes != "
                f"source {src_path} {len(src_bytes)} bytes"
            )

    if missing:
        print("check_web_gzip_parity: some assets could not be compared (informational):")
        for m in missing:
            print(f"  {m}")

    if compared == 0 and not mismatches:
        print(
            "SKIP: check_web_gzip_parity: no asset/.gz pair could be located to compare "
            f"(checked {len(assets)} asset(s) against {build_gz_dir})"
        )
        return 3

    if mismatches:
        print(f"check_web_gzip_parity: {len(mismatches)} mismatch(es) found:")
        for m in mismatches:
            print(f"  {m}")
        print(
            "  An embedded .gz decompresses to content different from its own source file -- "
            "this means the .gz is stale or was hand-edited. Reconfigure/rebuild KilnFW so "
            "CMakeLists.txt regenerates it from the current source."
        )
        return 1

    print(
        f"check_web_gzip_parity: OK -- {compared}/{len(assets)} embedded .gz asset(s) "
        "decompress to byte-identical content of their source."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
