# Point KilnCtrl-latest.elf at the ELF that was just linked. Invoked as a
# POST_BUILD step from CMakeLists.txt on every `idf.py build` -- see the long
# comment there for why the *canonical, hash-keyed* archive exists at all
# (build_info.h carries a build timestamp, so the ELF matching a flashed
# image can never be rebuilt).
#
# 2026-09-10: this step used to ALSO copy a permanent, content-hash-keyed
# `KilnCtrl-<hash>.elf` into this same directory on every ordinary build,
# whether or not that binary was ever flashed -- cmake has no way to know.
# That is the actual archiving mechanism's job now: `archive_kiln_elf()`
# (tools/PcTools/src/kilnctrl/elf_archive.py), called by flash_firmware()/
# debug_program() only after a CONFIRMED flash, registers the ELF in
# manifest.json keyed by the identity the board itself will later report
# (fw_build). Depositing an unregistered copy here on every local build grew
# the directory to ~1.3 GB / 68 files (2026-09-10 audit) for builds that were
# never on any board -- unreachable by lookup, and (until b693f6f1's
# adoption/superseded machinery) invisible to pruning too. This step now
# only maintains the `-latest.elf` convenience copy (overwritten in place,
# so it never accumulates) for the common "decode the dump from the build I
# just linked, before it's even flashed" case; it does not touch the
# manifest and is not a substitute for the real archive.
#
# Expects: -DELF=<path to the linked elf> -DOUTDIR=<archive directory>

if(NOT DEFINED ELF OR NOT EXISTS "${ELF}")
    # Nothing to archive. Deliberately not an error: failing the build over a
    # diagnostic convenience would be a worse outcome than not having it.
    return()
endif()

file(MAKE_DIRECTORY "${OUTDIR}")

# A pointer to the most recently linked build, so the common case ("decode
# the dump from the build I just linked") needs no hunting. Overwritten in
# place every build -- this is the ONLY file this step writes.
configure_file("${ELF}" "${OUTDIR}/KilnCtrl-latest.elf" COPYONLY)
