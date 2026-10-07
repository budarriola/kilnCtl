#include "cfg_fs_format_gate.h"

#include <stdio.h>
#include <string.h>

static const uint8_t kLittleFsMagic[8] = { 'l', 'i', 't', 't', 'l', 'e', 'f', 's' };

/* LittleFS on-disk constants, reproduced from the pinned
 * `joltwallet/littlefs` component (`managed_components/joltwallet__littlefs/
 * src/littlefs/lfs.h`) -- see cfg_fs_format_gate.h's file banner for the
 * exact citations. Not #included directly: that header pulls in the full
 * ESP-IDF LittleFS driver, which this pure/host-tested module must not
 * depend on. */
#define LFS_TAG_VALID_BIT       0x80000000u
#define LFS_TAG_TYPE3(tag)      (((tag) >> 20) & 0x7ffu)
#define LFS_TAG_ID(tag)         (((tag) >> 10) & 0x3ffu)
#define LFS_TAG_SIZE(tag)       ((tag) & 0x3ffu)
#define LFS_TAG_DSIZE(tag)      (4u + LFS_TAG_SIZE(tag))

#define LFS_TYPE_SUPERBLOCK     0x0ffu
#define LFS_TYPE_INLINESTRUCT   0x201u
#define LFS_TYPE_CCRC           0x500u
#define LFS_TYPE_CCRC_MASK      0x7feu /* CCRC's low bit ("eperturb") varies -- mask it off when comparing */

#define LFS_DISK_VERSION_MAJOR  2u
#define LFS_DISK_VERSION_MINOR  1u

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* CRC-32 (reflected, polynomial 0xedb88320), reproduced bit-for-bit from the
 * pinned component's `lfs_util.c` `lfs_crc()` -- the small-lookup-table
 * software implementation LittleFS uses to protect every commit. Must stay
 * byte-identical to that function or a genuinely valid filesystem's commits
 * will fail to validate here. */
static uint32_t lfs_style_crc(uint32_t crc, const uint8_t *data, size_t size)
{
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }
    return crc;
}

typedef enum {
    BLOCK_NO_SUPERBLOCK = 0, /* tag chain never reached a SUPERBLOCK tag -- no evidence of a filesystem here */
    BLOCK_SUPERBLOCK_VALID,  /* SUPERBLOCK+INLINESTRUCT reached, version sane, enclosing commit's CRC matched */
    BLOCK_SUPERBLOCK_CORRUPT,/* SUPERBLOCK tag reached via a real decoded chain, but the commit's CRC did not
                               * match, or the version/structure that followed it was not sane -- a genuine
                               * (corrupt) filesystem, not coincidental bytes */
} block_verdict_t;

/* Walks one LittleFS metadata block's tag chain exactly as
 * lfs_dir_fetchmatch() does (lfs.c): a 4-byte LE revision count, then a
 * stream of 4-byte big-endian tags each XORed against the previous tag,
 * each followed by lfs_tag_size(tag) bytes of data, running CRC-32'd from
 * the revision count onward and checked against the trailing LFS_TYPE_CCRC
 * tag's stored value. Stops at the first structurally invalid tag, at the
 * block boundary, or once a commit's CRC has been checked (we only need to
 * know whether the superblock commit itself is genuine, not walk every
 * later commit in the block). */
static block_verdict_t scan_metadata_block(const uint8_t *block, size_t block_size)
{
    if (block_size < CFG_FS_FORMAT_GATE_BLOCK_SIZE) {
        return BLOCK_NO_SUPERBLOCK;
    }

    uint32_t crc = lfs_style_crc(0xffffffffu, block, 4); /* over the revision count's raw on-disk bytes */
    uint32_t ptag = 0xffffffffu;
    size_t off = 4;

    bool have_superblock_tag = false;
    bool have_inlinestruct = false;
    uint32_t superblock_version = 0;

    while (off + 4 <= block_size) {
        uint32_t raw = rd_be32(block + off);
        crc = lfs_style_crc(crc, block + off, 4);
        uint32_t tag = raw ^ ptag;

        if (tag & LFS_TAG_VALID_BIT) {
            /* Erased (0xFF...) or otherwise unprogrammed -- end of this block's committed data. */
            break;
        }
        size_t dsize = LFS_TAG_DSIZE(tag);
        if (off + dsize > block_size) {
            break;
        }
        ptag = tag;

        uint32_t type3 = LFS_TAG_TYPE3(tag);
        if ((type3 & LFS_TYPE_CCRC_MASK) == LFS_TYPE_CCRC) {
            /* The CRC word always immediately follows the 4-byte tag header,
             * regardless of the tag's declared size (which encodes trailing
             * padding, not the CRC word's own length) -- see
             * lfs_dir_fetchmatch()'s identical off+sizeof(tag) read. */
            if (off + 8 > block_size) {
                break;
            }
            uint32_t dcrc = rd_le32(block + off + 4);
            if (crc != dcrc) {
                /* Commit is present but corrupt. If we'd already decoded a
                 * plausible superblock tag chain inside this same commit,
                 * that is a real (if damaged) filesystem, not noise. */
                return have_superblock_tag ? BLOCK_SUPERBLOCK_CORRUPT : BLOCK_NO_SUPERBLOCK;
            }
            if (have_superblock_tag && have_inlinestruct) {
                bool version_ok = ((superblock_version >> 16) == LFS_DISK_VERSION_MAJOR) &&
                                   ((superblock_version & 0xffffu) <= LFS_DISK_VERSION_MINOR);
                return version_ok ? BLOCK_SUPERBLOCK_VALID : BLOCK_SUPERBLOCK_CORRUPT;
            }
            /* A validly-CRC'd commit that isn't the superblock commit (e.g.
             * this block's very first commit on a filesystem that has since
             * been compacted) -- keep looking at the next commit for the
             * superblock tag, resetting the running CRC the same way
             * lfs_dir_fetchmatch() does between commits. */
            crc = 0xffffffffu;
            off += dsize;
            continue;
        }

        const uint8_t *data = block + off + 4;
        size_t data_len = dsize - 4;
        crc = lfs_style_crc(crc, data, data_len);

        if (type3 == LFS_TYPE_SUPERBLOCK && LFS_TAG_ID(tag) == 0 && data_len == sizeof(kLittleFsMagic) &&
            memcmp(data, kLittleFsMagic, sizeof(kLittleFsMagic)) == 0) {
            have_superblock_tag = true;
        } else if (type3 == LFS_TYPE_INLINESTRUCT && LFS_TAG_ID(tag) == 0 && data_len >= 4) {
            have_inlinestruct = true;
            superblock_version = rd_le32(data); /* lfs_superblock_t.version is the struct's first LE32 field */
        }

        off += dsize;
    }

    return have_superblock_tag ? BLOCK_SUPERBLOCK_CORRUPT /* superblock tag seen but its commit never closed
                                                            * with a matching CRC before the chain ended */
                                : BLOCK_NO_SUPERBLOCK;
}

void cfg_fs_format_gate_reset(cfg_fs_format_gate_t *gate)
{
    if (!gate) {
        return;
    }
    memset(gate, 0, sizeof(*gate));
}

void cfg_fs_format_gate_feed(cfg_fs_format_gate_t *gate, const uint8_t *chunk, size_t len)
{
    if (!gate || !chunk || len == 0) {
        return;
    }

    if (gate->header_len < sizeof(gate->header)) {
        size_t room = sizeof(gate->header) - gate->header_len;
        size_t take = len < room ? len : room;
        memcpy(gate->header + gate->header_len, chunk, take);
        gate->header_len += take;
    }

    for (size_t i = 0; i < len; i++) {
        if (chunk[i] != 0xFF) {
            gate->nonerased_bytes++;
        }
    }
    gate->total_bytes += len;
}

cfg_fs_format_gate_verdict_t cfg_fs_format_gate_conclude(const cfg_fs_format_gate_t *gate)
{
    if (!gate || gate->total_bytes == 0) {
        /* Nothing was scanned at all -- refuse rather than guess "blank".
         * A caller that failed to feed any bytes (e.g. the partition could
         * not be read back for some reason) must not have that silently
         * treated as evidence of nothing being there. */
        return CFG_FS_FORMAT_GATE_HAS_CONTENT;
    }
    if (gate->header_len < sizeof(gate->header)) {
        /* Fewer than two full blocks were captured -- cannot structurally
         * validate a superblock pair at all. Refuse rather than guess, same
         * reasoning as the zero-bytes case above. */
        return CFG_FS_FORMAT_GATE_HAS_CONTENT;
    }

    block_verdict_t v0 = scan_metadata_block(gate->header, CFG_FS_FORMAT_GATE_BLOCK_SIZE);
    block_verdict_t v1 = scan_metadata_block(gate->header + CFG_FS_FORMAT_GATE_BLOCK_SIZE,
                                              CFG_FS_FORMAT_GATE_BLOCK_SIZE);

    if (v0 == BLOCK_SUPERBLOCK_VALID || v1 == BLOCK_SUPERBLOCK_VALID ||
        v0 == BLOCK_SUPERBLOCK_CORRUPT || v1 == BLOCK_SUPERBLOCK_CORRUPT) {
        /* Either a fully valid superblock, or one that decoded far enough to
         * be recognizably real but failed its CRC/version check -- both are
         * a genuine (if possibly damaged) filesystem, never auto-formatted. */
        return CFG_FS_FORMAT_GATE_HAS_CONTENT;
    }

    /* No valid LittleFS superblock structure in either metadata block.
     * Byte density is deliberately NOT checked here -- leftover non-erased
     * flash that never formed a filesystem is exactly the case the owner
     * decision says must be auto-formatted, however much of the partition
     * it fills. */
    return CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT;
}

void cfg_fs_format_gate_describe(const cfg_fs_format_gate_t *gate, cfg_fs_format_gate_verdict_t verdict,
                                  char *buf, size_t buf_cap)
{
    if (!buf || buf_cap == 0) {
        return;
    }
    if (!gate || gate->total_bytes == 0) {
        snprintf(buf, buf_cap, "partition could not be scanned");
        return;
    }

    /* Tenths-of-a-percent non-erased, reported for operator context only --
     * it no longer gates the verdict (see cfg_fs_format_gate_conclude()). */
    uint64_t per_mille = ((uint64_t)gate->nonerased_bytes * 1000ULL) / (uint64_t)gate->total_bytes;

    if (verdict == CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT) {
        if (gate->nonerased_bytes == 0) {
            snprintf(buf, buf_cap, "no valid LittleFS superblock found (region reads as erased)");
        } else {
            snprintf(buf, buf_cap,
                     "no valid LittleFS superblock found (region was %u.%u%% non-erased data, not a filesystem)",
                     (unsigned)(per_mille / 10), (unsigned)(per_mille % 10));
        }
        return;
    }

    if (gate->header_len < sizeof(gate->header)) {
        snprintf(buf, buf_cap, "partition too short to hold a superblock (only %u byte(s) read back)",
                 (unsigned)gate->header_len);
        return;
    }

    block_verdict_t v0 = scan_metadata_block(gate->header, CFG_FS_FORMAT_GATE_BLOCK_SIZE);
    block_verdict_t v1 = scan_metadata_block(gate->header + CFG_FS_FORMAT_GATE_BLOCK_SIZE,
                                              CFG_FS_FORMAT_GATE_BLOCK_SIZE);
    if (v0 == BLOCK_SUPERBLOCK_VALID || v1 == BLOCK_SUPERBLOCK_VALID) {
        snprintf(buf, buf_cap, "LittleFS superblock signature found and its structure validates");
    } else {
        snprintf(buf, buf_cap,
                 "LittleFS superblock signature found but its commit failed CRC/version validation "
                 "(corrupt filesystem)");
    }
}

cfg_fs_confirm_decision_t cfg_fs_confirm_decide(bool mounted, bool skipped_for_recovery,
                                                              bool force_healthy)
{
    if (skipped_for_recovery) {
        return CFG_FS_FORMAT_CONFIRM_REFUSE_RECOVERY;
    }
    if (mounted && !force_healthy) {
        return CFG_FS_FORMAT_CONFIRM_REFUSE_HEALTHY;
    }
    return CFG_FS_FORMAT_CONFIRM_ALLOW;
}
