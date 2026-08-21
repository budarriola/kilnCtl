# Archive a freshly-linked ELF under build/elf_archive/, keyed by its hash.
#
# Invoked as a POST_BUILD step from CMakeLists.txt -- see the long comment
# there for why this exists (build_info.h carries a build timestamp, so the
# ELF matching a flashed image can never be rebuilt, and without it a coredump
# from that image cannot be symbolized).
#
# Expects: -DELF=<path to the linked elf> -DOUTDIR=<archive directory>

if(NOT DEFINED ELF OR NOT EXISTS "${ELF}")
    # Nothing to archive. Deliberately not an error: failing the build over a
    # diagnostic convenience would be a worse outcome than not having it.
    return()
endif()

file(MAKE_DIRECTORY "${OUTDIR}")

# The archive key is a hash of the ELF itself, not of the .bin the coredump
# actually records. Those are different values, so this name is an INDEX, not
# something to match against a dump's SHA directly. Identical rebuilds hash
# the same and collapse to one file; any change produces a new entry, which is
# all that is needed to guarantee the matching ELF still exists somewhere.
file(SHA256 "${ELF}" elf_hash)
string(SUBSTRING "${elf_hash}" 0 12 elf_key)

set(dest "${OUTDIR}/KilnCtrl-${elf_key}.elf")
if(NOT EXISTS "${dest}")
    configure_file("${ELF}" "${dest}" COPYONLY)
endif()

# A pointer to the most recent one, so the common case ("decode the dump from
# the build I just flashed") needs no hunting.
configure_file("${ELF}" "${OUTDIR}/KilnCtrl-latest.elf" COPYONLY)

# Newest-first listing, so picking the right ELF after several rebuilds does
# not mean reading file timestamps by hand.
file(GLOB archived "${OUTDIR}/KilnCtrl-*.elf")
list(REMOVE_ITEM archived "${OUTDIR}/KilnCtrl-latest.elf")
list(LENGTH archived archived_count)
message(STATUS "elf_archive: kept ${dest} (${archived_count} archived build(s))")
