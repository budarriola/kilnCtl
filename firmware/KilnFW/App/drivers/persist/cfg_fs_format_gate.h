// cfg_fs_format_gate -- pure, host-testable decision of whether a `cfg`
// LittleFS partition that failed to mount is safe to auto-format, per the
// owner decision (docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 1) as
// refined 2026-09-07: "The auto check should be looking to see if it is a
// valid file system, not just data. If it is just data and not file system
// then just format it."
//
// The question this module answers is narrow and structural: **is there a
// valid LittleFS filesystem in the first two blocks of this region?**
// LittleFS keeps its root superblock in a redundant metadata-block pair --
// by convention blocks 0 and 1 of the filesystem -- each block holding a
// revision count followed by a stream of tagged, CRC-protected commits (see
// the pinned `joltwallet/littlefs` component,
// `firmware/KilnFW/managed_components/joltwallet__littlefs/src/littlefs/`).
// This gate replays that on-disk format directly:
//
//   - `lfs.c`'s `lfs_dir_fetchmatch()` (metadata block parsing: 4-byte LE
//     revision count, then a chain of 4-byte big-endian tags XORed against
//     the previous tag, each followed by `lfs_tag_size(tag)` bytes of data)
//     and `lfs_dir_commitcrc()` (each commit ends with an `LFS_TYPE_CCRC`
//     tag carrying a CRC-32 over every tag+data byte since the block's
//     revision count -- computed with the exact table-based CRC-32 in
//     `lfs_util.c`'s `lfs_crc()`, reproduced here bit-for-bit) together
//     define how a genuine commit is distinguished from arbitrary bytes.
//   - `lfs_format_()` (`lfs.c`) shows the exact tag sequence LittleFS writes
//     for its superblock: an `LFS_TYPE_SUPERBLOCK` tag (id 0, size 8, data
//     `"littlefs"`) immediately followed by an `LFS_TYPE_INLINESTRUCT` tag
//     (id 0, size 24) holding an `lfs_superblock_t`
//     (version/block_size/block_count/name_max/file_max/attr_max, each
//     LE32), all inside one CRC-protected commit.
//   - `LFS_DISK_VERSION` (`lfs.h`, `0x00020001`) and the mount-time check in
//     `lfs.c` (`lfs_fs_rawmount`, ~line 4215: major must equal
//     `LFS_DISK_VERSION_MAJOR` and minor must be `<=
//     LFS_DISK_VERSION_MINOR`) define what counts as a sane version field.
//
// A region is judged to have a **valid filesystem** only when a block's tag
// chain decodes coherently far enough to reach a SUPERBLOCK tag with the
// right id/size, an immediately-following INLINESTRUCT tag with a
// plausible version, and the enclosing commit's CRC-32 actually matches.
// That is a very different bar from "the 8 bytes `littlefs` appear
// somewhere in this buffer" -- a coincidental byte match that is not
// reached by walking a real, CRC-validated tag chain is exactly the
// "leftover flash is not a filesystem" case the owner called out, and must
// NOT block formatting.
//
// Verdicts:
//   - CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT: no valid LittleFS superblock found
//     in either metadata block. This covers both an (almost) entirely
//     erased region AND a region full of non-erased residual bytes that do
//     not form a real filesystem (the bench board's case, 2026-09-07:
//     partition reads 86.6% non-erased -- leftover data from before the
//     `cfg` partition existed -- with no LittleFS structure at all).
//     Auto-format is safe regardless of how much non-erased data is
//     present; byte density is no longer a gating signal at all (the old
//     CFG_FS_FORMAT_GATE_NONERASED_PER_MILLE_THRESHOLD has been removed).
//   - CFG_FS_FORMAT_GATE_HAS_CONTENT: a real LittleFS superblock structure
//     was found (fully valid, or magic+chain reached but CRC/version
//     failed -- i.e. a genuine filesystem that is itself corrupt). Either
//     way this is exactly the case that must never be silently erased --
//     the caller (cfg_fs_mount.c) refuses to format, sets the pending
//     flag, and requires an explicit operator confirmation via
//     `/api/cfgfs/format_pending` + `/api/cfgfs/format_confirm`.
//
// Callers feed the partition's raw bytes through cfg_fs_format_gate_feed()
// in whatever chunk size suits them (the whole 512 KiB `cfg` partition is
// too large to hold in RAM at once on-device); only the first
// CFG_FS_FORMAT_GATE_HEADER_BYTES are actually retained (two metadata
// blocks' worth, matching the partition's configured LittleFS block size --
// CFG_FS_FORMAT_GATE_BLOCK_SIZE, 4096, the esp_littlefs default and the
// same size cfg_fs_mount.c already reads in). The remainder of the scan is
// still tallied for the informational non-erased-byte percentage reported
// in cfg_fs_format_gate_describe(), but it no longer gates the verdict.
#ifndef CFG_FS_FORMAT_GATE_H
#define CFG_FS_FORMAT_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT = 0, /* no valid LittleFS superblock found -- auto-format is safe */
    CFG_FS_FORMAT_GATE_HAS_CONTENT,        /* a real (possibly corrupt) LittleFS superblock structure was
                                             * found -- must NOT auto-format, ask instead */
} cfg_fs_format_gate_verdict_t;

/* LittleFS's configured on-disk block size for the `cfg` partition. Matches
 * esp_littlefs's default (the underlying flash sector size) and
 * cfg_fs_mount.c's CFG_FS_SCAN_CHUNK_BYTES -- the metadata block pair that
 * carries the superblock is always blocks 0 and 1, i.e. the first two
 * block-size regions of the partition. */
#define CFG_FS_FORMAT_GATE_BLOCK_SIZE 4096u

/* How many leading bytes of the region are retained verbatim for structural
 * parsing -- exactly the two candidate superblock metadata blocks. */
#define CFG_FS_FORMAT_GATE_HEADER_BYTES (2u * CFG_FS_FORMAT_GATE_BLOCK_SIZE)

typedef struct {
    size_t   total_bytes;
    size_t   nonerased_bytes; /* bytes seen so far that were not 0xFF -- informational only, does not gate */
    uint8_t  header[CFG_FS_FORMAT_GATE_HEADER_BYTES]; /* verbatim copy of the region's first two blocks */
    size_t   header_len;      /* bytes captured into `header` so far, saturates at sizeof(header) */
} cfg_fs_format_gate_t;

void cfg_fs_format_gate_reset(cfg_fs_format_gate_t *gate);
void cfg_fs_format_gate_feed(cfg_fs_format_gate_t *gate, const uint8_t *chunk, size_t len);
cfg_fs_format_gate_verdict_t cfg_fs_format_gate_conclude(const cfg_fs_format_gate_t *gate);

/* Human-readable reason string for the verdict -- surfaced verbatim in the
 * boot log and the web UI's confirmation banner, e.g. "LittleFS superblock
 * signature found and its structure is valid", "LittleFS superblock found
 * but its commit failed CRC validation (corrupt filesystem)", or "no valid
 * LittleFS superblock found (region was 86.6% non-erased)". Writes into
 * `buf` (capacity `buf_cap`), always NUL-terminated. */
void cfg_fs_format_gate_describe(const cfg_fs_format_gate_t *gate, cfg_fs_format_gate_verdict_t verdict,
                                  char *buf, size_t buf_cap);

/* POST /api/cfgfs/format_confirm decision (pure, host-tested). Since the NVS
 * dual-write close (docs/CONFIG_FILESYSTEM.md) a mounted `cfg` partition is
 * the ONLY writable copy of zones/profiles/preferences, so formatting a
 * healthy one loses data nothing else mirrors. The route therefore refuses
 * unless cfg is genuinely unusable (not mounted: the needs-format path this
 * route exists for), or the caller passed the explicit override
 * `force_healthy=1`. Recovery mode skipped the mount on purpose and is
 * refused unconditionally, override or not (the partition still holds the
 * config). Recovery takes precedence over the healthy check. */
typedef enum {
    CFG_FS_FORMAT_CONFIRM_ALLOW = 0,        /* go ahead and format */
    CFG_FS_FORMAT_CONFIRM_REFUSE_RECOVERY,  /* recovery mode skipped the mount -- never format */
    CFG_FS_FORMAT_CONFIRM_REFUSE_HEALTHY,   /* mounted and usable, no override -- refuse */
} cfg_fs_confirm_decision_t;

cfg_fs_confirm_decision_t cfg_fs_confirm_decide(bool mounted, bool skipped_for_recovery,
                                                              bool force_healthy);

/* True only when the request URI's query string carries the parameter
 * `force_healthy` with the exact value "1" (the first occurrence of the key
 * decides). Anything else -- no query, key absent, empty/other/percent-encoded
 * value, NULL -- is false: fail-closed, no override. Reads the URI in place so
 * the httpd handler needs no stack copy of the query string. */
bool cfg_fs_confirm_uri_force_healthy(const char *uri);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_FORMAT_GATE_H
