"""Archive a shipping build's ELF into firmware/KilnFW/elf_archive (called by make_release.ps1).

Usage: archive_release_elf.py <KilnCtrl.elf> <KilnCtrl.bin> <commit> <tag>
Exits non-zero when the ELF cannot be archived, so a release never ships an
image whose coredumps could not be decoded later."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "PcTools", "src"))


def archive(elf: str, app_bin: str, commit: str, tag: str) -> str:
    from kilnctrl import elf_archive, esp_app_desc
    desc = esp_app_desc.parse_app_desc_file(app_bin)
    res = elf_archive.archive_kiln_elf(elf, desc.build_timestamp, commit, f"make_release:{tag}")
    if not res.archived_path or not os.path.isfile(res.archived_path):
        raise RuntimeError(f"archive reported {res.archived_path!r} but the file is not there")
    return f"release ELF archived: {res.archived_path} (key {res.elf_key})"


def main(argv: list) -> int:
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        print(archive(*argv[1:]))
    except Exception as exc:  # noqa: BLE001
        print(f"ERROR: could not archive the release ELF: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
