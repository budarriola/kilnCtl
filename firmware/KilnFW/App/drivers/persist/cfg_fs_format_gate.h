// cfg_fs_format_gate -- pure, host-testable decision of whether a `cfg`
// LittleFS partition that failed to mount is safe to auto-format, per the
// owner decision (docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 1,
// "auto format, don't require all-FF, search for valid files/partitions,
// ask the user if it is ok to overwrite if partitions/files found"):
//
//   - A region that is (almost) entirely erased (0xFF) is safe to format
//     automatically -- this is the expected state of a `cfg` partition that
//     was flashed but has never been written, and IS today's real state on
//     hardware.
//   - A region that shows a recognizable LittleFS superblock magic, or any
//     non-trivial amount of non-erased data, must NOT be auto-formatted --
//     that is exactly the "corrupted but populated" filesystem the owner
//     decision says must never be silently destroyed. The caller (device
//     glue in cfg_fs_mount.c) surfaces this as a refusal requiring explicit
//     confirmation, never formats on its own.
//
// Deliberately NOT gated on "every single byte is 0xFF": a handful of bit
// errors in an otherwise-blank region must not force a false "has content"
// refusal forever (nothing could ever un-stick it without a manual erase),
// so a small fraction of stray non-erased bytes still reads as blank. The
// LittleFS superblock magic ("littlefs", the 8-byte tag LittleFS writes at
// a fixed offset in both of its superblock's paired metadata blocks) is
// checked independently and OVERRIDES the byte-fraction heuristic --
// finding it anywhere means real structure exists, full stop, regardless of
// how small the buffer scanned so far is.
//
// Callers feed the partition's raw bytes through cfg_fs_format_gate_feed()
// in whatever chunk size suits them (the whole 512 KiB `cfg` partition is
// too large to hold in RAM at once on-device) and read the verdict from
// cfg_fs_format_gate_conclude() once every byte has been fed.
#ifndef CFG_FS_FORMAT_GATE_H
#define CFG_FS_FORMAT_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT = 0, /* no evidence of real content -- auto-format is safe */
    CFG_FS_FORMAT_GATE_HAS_CONTENT,        /* evidence found -- must NOT auto-format, ask instead */
} cfg_fs_format_gate_verdict_t;

typedef struct {
    size_t   total_bytes;
    size_t   nonerased_bytes;  /* bytes seen so far that were not 0xFF */
    bool     magic_found;      /* the LittleFS superblock magic was seen in the stream */
    uint8_t  tail[7];          /* last <=7 bytes of the previous chunk, for magic matches
                                 * that straddle a chunk boundary */
    size_t   tail_len;
} cfg_fs_format_gate_t;

/* Fraction of non-erased bytes (in tenths of a percent, i.e. out of 1000)
 * above which the region is judged to have real content even without a
 * superblock magic match -- a handful of stray bits in an otherwise blank
 * 512 KiB region is normal flash wear, not a filesystem. 1 (0.1%) of a
 * 512 KiB partition is ~512 bytes -- far more than bit-error noise, far
 * less than even a single small JSON file's worth of real data. */
#define CFG_FS_FORMAT_GATE_NONERASED_PER_MILLE_THRESHOLD 1

void cfg_fs_format_gate_reset(cfg_fs_format_gate_t *gate);
void cfg_fs_format_gate_feed(cfg_fs_format_gate_t *gate, const uint8_t *chunk, size_t len);
cfg_fs_format_gate_verdict_t cfg_fs_format_gate_conclude(const cfg_fs_format_gate_t *gate);

/* Human-readable reason string for the verdict -- surfaced verbatim in the
 * boot log and the web UI's confirmation banner, e.g. "LittleFS superblock
 * signature found" or "3.2% of the partition is non-erased data". Writes
 * into `buf` (capacity `buf_cap`), always NUL-terminated. */
void cfg_fs_format_gate_describe(const cfg_fs_format_gate_t *gate, cfg_fs_format_gate_verdict_t verdict,
                                  char *buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_FORMAT_GATE_H
