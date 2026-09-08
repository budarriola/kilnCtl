// Host tests for cfg_fs_format_gate.c -- the pure "valid LittleFS filesystem
// vs just data" decision behind the owner decision, refined 2026-09-07:
// "The auto check should be looking to see if it is a valid file system,
// not just data. If it is just data and not file system then just format
// it." Pure stdint/string only, no ESP-IDF -- exercised directly, byte
// buffers standing in for the partition reads cfg_fs_mount.c does on real
// hardware.
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/persist/cfg_fs_format_gate.h"

/* ---- Minimal, spec-accurate LittleFS metadata block construction --------
 * Builds a metadata block exactly the way lfs_format_() (lfs.c) does for a
 * fresh filesystem's superblock commit: a 4-byte LE revision count, then
 * CREATE(size 0), SUPERBLOCK(id 0, size 8, "littlefs"),
 * INLINESTRUCT(id 0, size 24, lfs_superblock_t), and a closing
 * LFS_TYPE_CCRC tag (id 0x3ff) whose 4-byte payload is the running CRC-32
 * over everything since the revision count -- computed here with the same
 * table-based CRC-32 cfg_fs_format_gate.c uses (reproduced from the pinned
 * component's lfs_util.c). This is a *reference* construction independent
 * of cfg_fs_format_gate.c's own implementation, so a passing test is
 * actually exercising the production CRC/tag-walk logic against real
 * on-disk bytes, not against itself. */

#define LFS_MKTAG(type, id, size) (((uint32_t)(type) << 20) | ((uint32_t)(id) << 10) | (uint32_t)(size))
#define LFS_TYPE_CREATE       0x401u
#define LFS_TYPE_SUPERBLOCK   0x0ffu
#define LFS_TYPE_INLINESTRUCT 0x201u
#define LFS_TYPE_CCRC         0x500u

static uint32_t ref_crc(uint32_t crc, const uint8_t *data, size_t size)
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

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)(v);
}

/* Appends one tag+data to `block` at *off, XORing against *ptag exactly as
 * lfs_dir_commitattr() does, and folds it into *crc. Returns the new tag
 * (caller passes it back in as ptag for the next append). */
static uint32_t append_tag(uint8_t *block, size_t *off, uint32_t ptag, uint32_t *crc,
                            uint32_t type, uint32_t id, const uint8_t *data, size_t size)
{
    uint32_t tag = LFS_MKTAG(type, id, (uint32_t)size);
    uint8_t raw[4];
    wr_be32(raw, tag ^ ptag);
    memcpy(block + *off, raw, 4);
    *crc = ref_crc(*crc, raw, 4);
    *off += 4;
    if (size > 0) {
        memcpy(block + *off, data, size);
        *crc = ref_crc(*crc, data, size);
        *off += size;
    }
    return tag;
}

/* Fills `block` (must be >= CFG_FS_FORMAT_GATE_BLOCK_SIZE) with a valid
 * superblock commit, erased (0xFF) beyond it. If `corrupt_crc` is set, the
 * final stored CRC word is deliberately wrong (simulates a filesystem whose
 * superblock commit was interrupted mid-write -- a real, if damaged,
 * filesystem, not coincidental bytes). */
static void build_superblock_block(uint8_t *block, bool corrupt_crc)
{
    memset(block, 0xFF, CFG_FS_FORMAT_GATE_BLOCK_SIZE);

    uint32_t rev = 1;
    wr_le32(block, rev);
    size_t off = 4;
    uint32_t crc = ref_crc(0xffffffffu, block, 4);
    uint32_t ptag = 0xffffffffu;

    ptag = append_tag(block, &off, ptag, &crc, LFS_TYPE_CREATE, 0, NULL, 0);
    ptag = append_tag(block, &off, ptag, &crc, LFS_TYPE_SUPERBLOCK, 0,
                       (const uint8_t *)"littlefs", 8);

    uint8_t superblock[24];
    memset(superblock, 0, sizeof(superblock));
    wr_le32(superblock + 0, 0x00020001u);  /* version: major 2, minor 1 -- LFS_DISK_VERSION */
    wr_le32(superblock + 4, CFG_FS_FORMAT_GATE_BLOCK_SIZE); /* block_size */
    wr_le32(superblock + 8, 128);           /* block_count (512 KiB / 4096) */
    wr_le32(superblock + 12, 255);          /* name_max */
    wr_le32(superblock + 16, 2147483647u);  /* file_max */
    wr_le32(superblock + 20, 1022);         /* attr_max */
    ptag = append_tag(block, &off, ptag, &crc, LFS_TYPE_INLINESTRUCT, 0, superblock, sizeof(superblock));

    /* Closing CCRC tag: id 0x3ff, size 4 (just the CRC word, no padding). */
    uint32_t ccrc_tag = LFS_MKTAG(LFS_TYPE_CCRC, 0x3ff, 4);
    uint8_t raw[4];
    wr_be32(raw, ccrc_tag ^ ptag);
    memcpy(block + off, raw, 4);
    crc = ref_crc(crc, raw, 4);
    off += 4;

    if (corrupt_crc) {
        crc ^= 0xFFFFFFFFu; /* deliberately wrong */
    }
    uint8_t crc_bytes[4];
    wr_le32(crc_bytes, crc);
    memcpy(block + off, crc_bytes, 4);
}

static void test_blank_partition_is_safe_to_format(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    for (int i = 0; i < 128; i++) { /* 128 * 4096 = 512 KiB, the real partition size */
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT,
               "an entirely-0xFF 512 KiB region is judged safe to auto-format");
}

static void test_a_few_stray_bits_still_reads_as_blank(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    chunk[10] = 0x00; /* one stray bit-error byte in the first chunk only */
    cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    memset(chunk, 0xFF, sizeof(chunk));
    for (int i = 0; i < 127; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT,
               "a single stray non-erased byte in 512 KiB does not by itself force a refusal");
}

static void test_residual_nonerased_data_without_a_filesystem_still_formats(void)
{
    /* The bench board's actual case, 2026-09-07: the `cfg` partition reads
     * 86.6% non-erased (leftover bytes from before the partition existed),
     * with no LittleFS structure anywhere in it. The owner decision is
     * explicit that byte density must not gate this -- "if it is just data
     * and not file system then just format it." */
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t chunk[4096];
    /* Deterministic "residual data" pattern -- not a repeating byte (which
     * could coincidentally decode as a run of same-typed tags), not 0xFF,
     * and containing no "littlefs" substring. */
    for (size_t i = 0; i < sizeof(chunk); i++) {
        chunk[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }
    size_t total = 0;
    size_t nonerased = 0;
    for (int i = 0; i < 128; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
        total += sizeof(chunk);
    }
    for (size_t i = 0; i < sizeof(chunk); i++) {
        if (chunk[i] != 0xFF) {
            nonerased++;
        }
    }
    /* Sanity: this pattern really is >80% non-erased, like the bench board. */
    TEST_CHECK(nonerased * 100 / sizeof(chunk) > 80, "test fixture reproduces a high non-erased density");
    (void)total;

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT,
               "a region that is mostly non-erased residual data but contains no valid LittleFS "
               "superblock is judged safe to auto-format -- density alone no longer refuses");
}

static void test_bare_magic_bytes_with_no_real_structure_still_formats(void)
{
    /* The exact regression this rewrite must avoid reintroducing: the old
     * gate treated the raw 8-byte string "littlefs" appearing ANYWHERE in
     * the buffer as proof of a filesystem. Planted with no preceding CREATE
     * tag, no CCRC, no real commit -- coincidental bytes, not a filesystem
     * -- must now format. */
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    memcpy(chunk + 32, "littlefs", 8);
    cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    memset(chunk, 0xFF, sizeof(chunk));
    for (int i = 0; i < 127; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_SAFE_TO_FORMAT,
               "a bare 'littlefs' byte string with no valid tag chain around it is not a filesystem "
               "and must be auto-formatted");
}

static void test_valid_superblock_refuses_and_is_never_formatted(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t block0[CFG_FS_FORMAT_GATE_BLOCK_SIZE];
    uint8_t block1[CFG_FS_FORMAT_GATE_BLOCK_SIZE];
    build_superblock_block(block0, false);
    memset(block1, 0xFF, sizeof(block1)); /* the sibling metadata block; irrelevant to this test */

    cfg_fs_format_gate_feed(&gate, block0, sizeof(block0));
    cfg_fs_format_gate_feed(&gate, block1, sizeof(block1));
    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    for (int i = 0; i < 126; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    cfg_fs_format_gate_verdict_t verdict = cfg_fs_format_gate_conclude(&gate);
    TEST_CHECK(verdict == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "a real, CRC-valid LittleFS superblock commit refuses to auto-format");

    char reason[128];
    cfg_fs_format_gate_describe(&gate, verdict, reason, sizeof(reason));
    TEST_CHECK(strstr(reason, "validates") != NULL, "the reason names the superblock as structurally valid");
}

static void test_corrupt_superblock_refuses_and_sets_pending(void)
{
    /* A valid-looking superblock whose commit CRC does not check out -- a
     * genuine filesystem that is itself corrupt (e.g. power loss mid-write
     * to the superblock). Per the owner's step 3, this must still refuse
     * and require explicit confirmation, never be silently erased. */
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t block0[CFG_FS_FORMAT_GATE_BLOCK_SIZE];
    uint8_t block1[CFG_FS_FORMAT_GATE_BLOCK_SIZE];
    build_superblock_block(block0, true /* corrupt CRC */);
    memset(block1, 0xFF, sizeof(block1));

    cfg_fs_format_gate_feed(&gate, block0, sizeof(block0));
    cfg_fs_format_gate_feed(&gate, block1, sizeof(block1));
    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    for (int i = 0; i < 126; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    cfg_fs_format_gate_verdict_t verdict = cfg_fs_format_gate_conclude(&gate);
    TEST_CHECK(verdict == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "a superblock commit with a mismatched CRC still refuses to auto-format");

    char reason[128];
    cfg_fs_format_gate_describe(&gate, verdict, reason, sizeof(reason));
    TEST_CHECK(strstr(reason, "CRC") != NULL || strstr(reason, "corrupt") != NULL,
               "the reason names the CRC/corruption failure, distinct from a fully-valid superblock");
}

static void test_magic_split_across_feed_calls_is_still_found(void)
{
    /* A real device reads the partition in fixed-size chunks -- a valid
     * superblock commit that happens to straddle a feed() boundary must
     * still be recognized once the header capture reassembles it. */
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t block0[CFG_FS_FORMAT_GATE_BLOCK_SIZE];
    build_superblock_block(block0, false);

    size_t split = 37; /* arbitrary split point inside the commit */
    cfg_fs_format_gate_feed(&gate, block0, split);
    cfg_fs_format_gate_feed(&gate, block0 + split, sizeof(block0) - split);

    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk)); /* block 1 */
    for (int i = 0; i < 126; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "a superblock commit split across two feed() chunks is still detected");
}

static void test_nothing_scanned_refuses_rather_than_guesses_blank(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);
    /* No feed() calls at all -- e.g. the partition could not be read back. */
    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "zero bytes scanned refuses to format rather than defaulting to safe-to-format");
}

static void test_too_short_to_hold_two_blocks_refuses(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);
    uint8_t chunk[100];
    memset(chunk, 0xFF, sizeof(chunk));
    cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "fewer than two full metadata blocks read back refuses rather than guessing blank");
}

void run_test_cfg_fs_format_gate(void)
{
    test_blank_partition_is_safe_to_format();
    test_a_few_stray_bits_still_reads_as_blank();
    test_residual_nonerased_data_without_a_filesystem_still_formats();
    test_bare_magic_bytes_with_no_real_structure_still_formats();
    test_valid_superblock_refuses_and_is_never_formatted();
    test_corrupt_superblock_refuses_and_sets_pending();
    test_magic_split_across_feed_calls_is_still_found();
    test_nothing_scanned_refuses_rather_than_guesses_blank();
    test_too_short_to_hold_two_blocks_refuses();
}
