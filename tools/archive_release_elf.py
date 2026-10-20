"""Archive a shipping build's ELF into firmware/KilnFW/elf_archive (called by make_release.ps1).

Usage: archive_release_elf.py <KilnCtrl.elf> <KilnCtrl.bin> <commit> <tag> [<recovery.bin>]
<commit> may be "unknown" or "external" when the build is not from HEAD (-SkipBuild/-BuildDir).
With <recovery.bin>, recovery.elf next to it is also archived (recovery_elf_archive, RA-02).
Exits non-zero when the ELF cannot be archived, so a release never ships an
image whose coredumps could not be decoded later."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "PcTools", "src"))


def archive_recovery(recovery_bin: str, commit: str) -> str:
    import hashlib
    from kilnctrl import elf_archive, esp_app_desc, recovery_flash
    desc = esp_app_desc.parse_app_desc_file(recovery_bin)
    with open(recovery_bin, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    image = recovery_flash.RecoveryImage(path=recovery_bin, size=os.path.getsize(recovery_bin),
                                         sha256=sha, app_desc=desc)
    adir = recovery_flash.recovery_archive_dir(elf_archive._repo_root())
    dest, note = recovery_flash.archive_recovery_elf(recovery_bin, adir, image, commit)
    if not dest or not os.path.isfile(dest):
        raise RuntimeError(f"recovery ELF not archived: {note}")
    return f"recovery ELF archived: {dest}"


def archive(elf: str, app_bin: str, commit: str, tag: str, recovery_bin: str = "") -> str:
    from kilnctrl import elf_archive, esp_app_desc
    desc = esp_app_desc.parse_app_desc_file(app_bin)
    res = elf_archive.archive_kiln_elf(elf, desc.build_timestamp, commit, f"make_release:{tag}")
    if not res.archived_path or not os.path.isfile(res.archived_path):
        raise RuntimeError(f"archive reported {res.archived_path!r} but the file is not there")
    msg = f"release ELF archived: {res.archived_path} (key {res.elf_key})"
    if recovery_bin:
        msg += " | " + archive_recovery(recovery_bin, commit)
    return msg


def main(argv: list) -> int:
    if len(argv) not in (5, 6):
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
