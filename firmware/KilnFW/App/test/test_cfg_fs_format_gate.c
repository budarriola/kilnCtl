// Host tests for cfg_fs_format_gate.c -- the pure blank-vs-populated
// decision behind the owner decision "auto format, don't require all-FF,
// search for valid files/partitions, ask the user if found"
// (docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 1). Pure stdint/string
// only, no ESP-IDF -- exercised directly, byte buffers standing in for the
// partition reads cfg_fs_mount.c does on real hardware.
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/persist/cfg_fs_format_gate.h"

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
               "a single stray non-erased byte in 512 KiB does not by itself force a refusal "
               "(the owner decision explicitly rejects an all-0xFF-only gate, in both directions: "
               "not requiring perfection, and not treating any imperfection as content)");
}

static void test_littlefs_superblock_magic_is_content(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));
    /* Plant the LittleFS superblock tag exactly as it would appear inside a
     * real, mount-failed-but-populated filesystem image. */
    memcpy(chunk + 32, "littlefs", 8);
    cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));

    cfg_fs_format_gate_verdict_t verdict = cfg_fs_format_gate_conclude(&gate);
    TEST_CHECK(verdict == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "a LittleFS superblock signature is recognized as evidence of real content");

    char reason[128];
    cfg_fs_format_gate_describe(&gate, verdict, reason, sizeof(reason));
    TEST_CHECK(strstr(reason, "superblock") != NULL, "the refusal reason names the superblock signature");
}

static void test_magic_split_across_feed_calls_is_still_found(void)
{
    /* Real device reads happen in fixed-size chunks -- the magic string
     * must still be detected when it straddles a chunk boundary, or a
     * caller that just happened to read in a size that split it would
     * silently auto-format a populated filesystem. */
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t first[4096];
    memset(first, 0xFF, sizeof(first));
    memcpy(first + 4092, "litt", 4); /* magic starts at the very end of chunk 1 */
    uint8_t second[4096];
    memset(second, 0xFF, sizeof(second));
    memcpy(second, "lefs", 4); /* and finishes at the very start of chunk 2 */

    cfg_fs_format_gate_feed(&gate, first, sizeof(first));
    cfg_fs_format_gate_feed(&gate, second, sizeof(second));

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "a superblock magic split across two feed() chunks is still detected");
}

static void test_substantial_nonerased_data_without_magic_is_content(void)
{
    /* Real file data (no superblock landed in the sampled bytes, e.g. this
     * chunk is pure JSON payload) must still be recognized as content by
     * the byte-fraction heuristic alone. */
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);

    uint8_t chunk[4096];
    memset(chunk, 0x41, sizeof(chunk)); /* 'A' -- plausible ASCII file content, no 0xFF at all */
    for (int i = 0; i < 128; i++) {
        cfg_fs_format_gate_feed(&gate, chunk, sizeof(chunk));
    }

    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "a region that is mostly non-erased data (no magic needed) is judged to have content");
}

static void test_nothing_scanned_refuses_rather_than_guesses_blank(void)
{
    cfg_fs_format_gate_t gate;
    cfg_fs_format_gate_reset(&gate);
    /* No feed() calls at all -- e.g. the partition could not be read back. */
    TEST_CHECK(cfg_fs_format_gate_conclude(&gate) == CFG_FS_FORMAT_GATE_HAS_CONTENT,
               "zero bytes scanned refuses to format rather than defaulting to safe-to-format");
}

void run_test_cfg_fs_format_gate(void)
{
    test_blank_partition_is_safe_to_format();
    test_a_few_stray_bits_still_reads_as_blank();
    test_littlefs_superblock_magic_is_content();
    test_magic_split_across_feed_calls_is_still_found();
    test_substantial_nonerased_data_without_magic_is_content();
    test_nothing_scanned_refuses_rather_than_guesses_blank();
}
