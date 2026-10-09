// test_update_task_erase_plan.c -- the slot-erase walk, after 2026-09-18
// dropped UPDATE_TASK_ERASE_CHUNK_SIZE from 64 KB to 4 KB
// (docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md).
//
// WHY THIS FILE EXISTS. The defect the audit confirmed was not an arithmetic
// bug: an ESP-driven Pico OTA hardware-watchdog-reset the RP2040 partway
// through erasing the destination slot, because hal_flash_safe_execute()
// disables interrupts on BOTH cores for the duration of one erase, so the
// watchdog task -- pinned to the other core -- cannot run to feed the 1000 ms
// hardware watchdog. A 64 KB block erase takes longer than that window. The
// remedy is a smaller erase unit plus a feed issued by the erasing task
// itself, between erases.
//
// That remedy changes two things worth testing separately:
//
//   1. The WALK. 13 chunks became 208. An off-by-one in a rewritten loop
//      either overruns the slot (erasing whatever follows it) or leaves a gap
//      (stale bytes inside a region update_task_program_chunk() afterwards
//      assumes reads back as 0xFF). Neither announces itself at erase time --
//      the update would simply verify wrong, or corrupt a neighbour, long
//      after the erase "succeeded". These tests drive the real production
//      arithmetic (update_task_erase_plan.c, linked in for real) and assert
//      exact, contiguous, gap-free, overrun-free coverage.
//
//   2. The two things the walk's own arithmetic CANNOT see: that
//      update_task.c actually asks for 4 KB chunks, and that its erase loop
//      actually issues the gated feed. Those live in update_task.c, which
//      pulls in FreeRTOS.h and pico headers and so cannot be host-compiled --
//      so they are source-text scans, with the usual caveat that a scan
//      proves presence, not behaviour. The behaviour of the gate itself is
//      tested for real in test_watchdog_gate.c.
//
// FAILS CLOSED: a missing or renamed source file is a TEST FAILURE, never a
// skip.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "tasks/update_task_erase_plan.h"

// bootloader/flash_layout.h is not on the host-test include path (it is a
// target-side header living beside the bootloader), so the slot size is
// restated here AND cross-checked against its definition by a source scan
// below -- restating a constant without pinning it to its source is how a
// test quietly stops describing the firmware it claims to test.
#define TEST_SLOT_FLASH_SIZE 0x000D0000u
#define TEST_ERASE_CHUNK_SIZE (4u * 1024u)

static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

static const char *UPDATE_TASK_CANDIDATES[] = {
    "../src/tasks/update_task.c",
    "src/tasks/update_task.c",
    "firmware/SaftyFW/src/tasks/update_task.c",
};

static const char *FLASH_LAYOUT_CANDIDATES[] = {
    "../bootloader/flash_layout.h",
    "bootloader/flash_layout.h",
    "firmware/SaftyFW/bootloader/flash_layout.h",
};

// 832 KB at 4 KB a chunk is 208 chunks exactly. This is the number the ESP
// side's RELAY_ERASE_TIMEOUT_MS is now derived from, so it is worth asserting
// rather than assuming.
static void test_slot_is_exactly_208_chunks(void)
{
    TEST_SECTION("update_erase_plan -- 0xD0000 at 4 KB is exactly 208 chunks, no remainder");

    uint32_t count = update_erase_plan_chunk_count(TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE);

    TEST_CHECK(count == 208u, "the 832 KB slot must take exactly 208 4 KB erases -- the ESP-side "
                               "RELAY_ERASE_TIMEOUT_MS derivation is stated in terms of this count");
    TEST_CHECK((TEST_SLOT_FLASH_SIZE % TEST_ERASE_CHUNK_SIZE) == 0u,
               "the slot size must divide evenly by the erase chunk size -- a remainder would "
               "mean a final short erase, which hal_flash_erase() rejects outright since it "
               "requires whole 4096-byte sectors");
}

// The actual defect class this file exists to catch: every byte of the slot
// erased exactly once, nothing outside it touched.
static void test_walk_covers_slot_exactly_no_gap_no_overrun(void)
{
    TEST_SECTION("update_erase_plan -- the 208-chunk walk covers the whole slot exactly");

    const uint32_t slot_offset = 0x00010000u; // a plausible non-zero slot base
    const uint32_t count = update_erase_plan_chunk_count(TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE);

    uint32_t expected_next = slot_offset;
    uint64_t total_erased = 0;
    bool all_ok = true;
    bool all_contiguous = true;
    bool all_aligned = true;
    bool all_in_bounds = true;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t offset = 0;
        uint32_t len = 0;
        if (!update_erase_plan_chunk(slot_offset, TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE, i,
                                      &offset, &len)) {
            all_ok = false;
            break;
        }
        if (offset != expected_next) {
            all_contiguous = false; // a gap (skipped bytes) or an overlap (double erase)
        }
        if ((offset % TEST_ERASE_CHUNK_SIZE) != 0u || len != TEST_ERASE_CHUNK_SIZE) {
            all_aligned = false; // hal_flash_erase() rejects non-sector-aligned offset/len
        }
        if (offset < slot_offset || (uint64_t)offset + len > (uint64_t)slot_offset + TEST_SLOT_FLASH_SIZE) {
            all_in_bounds = false; // would erase outside the destination slot
        }
        expected_next = offset + len;
        total_erased += len;
    }

    TEST_CHECK(all_ok, "every chunk index below the chunk count must describe a real chunk");
    TEST_CHECK(all_contiguous, "chunk N must start exactly where chunk N-1 ended -- a gap leaves "
                                "stale bytes that update_task_program_chunk() later assumes read "
                                "back as 0xFF, an overlap erases the same sector twice");
    TEST_CHECK(all_aligned, "every chunk must be a whole 4096-byte sector at a 4096-byte "
                             "boundary, since hal_flash_erase() rejects anything else");
    TEST_CHECK(all_in_bounds, "no chunk may reach outside the destination slot -- an overrun "
                               "erases whatever follows it in flash");
    TEST_CHECK(expected_next == slot_offset + TEST_SLOT_FLASH_SIZE,
               "the walk must end exactly on the slot's last byte, not short of it and not past it");
    TEST_CHECK(total_erased == (uint64_t)TEST_SLOT_FLASH_SIZE,
               "the erased byte total must equal the slot size exactly");
}

// Fail-closed at the boundary: the loop must not be able to walk off the end
// and erase a guessed range.
static void test_index_past_end_is_refused(void)
{
    TEST_SECTION("update_erase_plan -- an index past the last chunk is refused, not guessed");

    const uint32_t count = update_erase_plan_chunk_count(TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE);
    uint32_t offset = 0xDEADBEEFu;
    uint32_t len = 0xDEADBEEFu;

    bool ok = update_erase_plan_chunk(0u, TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE, count,
                                       &offset, &len);

    TEST_CHECK(!ok, "index == chunk_count is one past the last chunk and must be refused");
    TEST_CHECK(offset == 0xDEADBEEFu && len == 0xDEADBEEFu,
               "a refused chunk must not write its outputs -- a caller that ignores the return "
               "value must not be handed a plausible-looking erase range");

    uint32_t last_offset = 0;
    uint32_t last_len = 0;
    bool last_ok = update_erase_plan_chunk(0u, TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE,
                                            count - 1u, &last_offset, &last_len);
    TEST_CHECK(last_ok, "the last valid index must still be accepted");
    TEST_CHECK(last_offset == TEST_SLOT_FLASH_SIZE - TEST_ERASE_CHUNK_SIZE,
               "the last chunk must start one chunk short of the end of the slot");
}

// A total that does not divide evenly must still be fully covered by a short
// final chunk rather than truncated. The slot size happens to divide evenly
// today, but the arithmetic is general and a future slot size might not.
static void test_short_tail_is_covered_not_truncated(void)
{
    TEST_SECTION("update_erase_plan -- an uneven total gets a short final chunk, not a truncated walk");

    const uint32_t total = 10000u; // 2 whole 4096 chunks + 1808 bytes
    uint32_t count = update_erase_plan_chunk_count(total, TEST_ERASE_CHUNK_SIZE);
    TEST_CHECK(count == 3u, "10000 bytes at 4096 must round UP to 3 chunks, not truncate to 2");

    uint64_t covered = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t offset = 0;
        uint32_t len = 0;
        if (update_erase_plan_chunk(0u, total, TEST_ERASE_CHUNK_SIZE, i, &offset, &len)) {
            covered += len;
        }
    }
    TEST_CHECK(covered == total, "the three chunks must sum to exactly 10000 bytes -- the final "
                                  "chunk is short (1808), never a full chunk running past the end");
}

static void test_degenerate_arguments_refused(void)
{
    TEST_SECTION("update_erase_plan -- degenerate arguments are refused rather than dividing by zero");

    TEST_CHECK(update_erase_plan_chunk_count(TEST_SLOT_FLASH_SIZE, 0u) == 0u,
               "a zero chunk size must report zero chunks, never divide by zero");
    TEST_CHECK(update_erase_plan_chunk_count(0u, TEST_ERASE_CHUNK_SIZE) == 0u,
               "a zero total must report zero chunks");

    uint32_t offset = 0;
    uint32_t len = 0;
    TEST_CHECK(!update_erase_plan_chunk(0u, TEST_SLOT_FLASH_SIZE, 0u, 0u, &offset, &len),
               "a zero chunk size must be refused");
    TEST_CHECK(!update_erase_plan_chunk(0u, TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE, 0u, NULL, &len),
               "a NULL out_offset must be refused");
    TEST_CHECK(!update_erase_plan_chunk(0u, TEST_SLOT_FLASH_SIZE, TEST_ERASE_CHUNK_SIZE, 0u, &offset, NULL),
               "a NULL out_len must be refused");
}

// --- source scans (update_task.c cannot be host-compiled) -------------------

// The chunk size is the whole point of the remedy: at 64 KB a single
// interrupts-disabled erase outlasts the 1000 ms watchdog, and no amount of
// feeding BETWEEN erases can help, because nothing runs during one.
static void test_update_task_uses_4k_erase_chunks(void)
{
    TEST_SECTION("update_task.c -- UPDATE_TASK_ERASE_CHUNK_SIZE is 4 KB, not 64 KB");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                                sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    TEST_CHECK(text != NULL, "update_task.c must be readable -- a missing source is a failure, "
                              "never a silent skip");
    if (!text) {
        return;
    }

    const char *def = strstr(text, "#define UPDATE_TASK_ERASE_CHUNK_SIZE");
    TEST_CHECK(def != NULL, "UPDATE_TASK_ERASE_CHUNK_SIZE must still be defined in update_task.c");
    if (def) {
        const char *eol = strchr(def, '\n');
        size_t line_len = eol ? (size_t)(eol - def) : strlen(def);
        char line[256];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1u;
        }
        memcpy(line, def, line_len);
        line[line_len] = '\0';

        TEST_CHECK(strstr(line, "(4u * 1024u)") != NULL,
                   "the erase chunk must be 4 KB: one 64 KB block erase holds both cores'"
                   " interrupts off for longer than the 1000 ms hardware watchdog, which is the "
                   "confirmed cause of the mid-erase reset (2026-09-18 audit)");
        TEST_CHECK(strstr(line, "64") == NULL,
                   "no 64 KB chunk size may remain on the define line");
    }

    free(text);
}

// The other half of the remedy. A smaller erase unit alone only shortens each
// blackout; without a feed issued from the erase loop, 208 blackouts back to
// back still starve the watchdog across the whole ~9 s erase.
static void test_erase_loop_feeds_through_the_shared_gate(void)
{
    TEST_SECTION("update_task.c -- the erase loop feeds via the SHARED gate, never unconditionally");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                                sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    TEST_CHECK(text != NULL, "update_task.c must be readable");
    if (!text) {
        return;
    }

    const char *fn = strstr(text, "static bool update_task_erase_slot(");
    TEST_CHECK(fn != NULL, "update_task_erase_slot() must still be defined where this scan expects it");

    if (fn) {
        // Bound the scan to this function's body so a call somewhere else in
        // the file cannot satisfy it.
        const char *next_fn = strstr(fn + 1, "\nstatic ");
        size_t body_len = next_fn ? (size_t)(next_fn - fn) : strlen(fn);
        char *body = (char *)malloc(body_len + 1u);
        TEST_CHECK(body != NULL, "test allocation must succeed");
        if (body) {
            memcpy(body, fn, body_len);
            body[body_len] = '\0';

            TEST_CHECK(strstr(body, "watchdog_task_feed_if_all_within_deadline") != NULL,
                       "the erase loop must feed the watchdog itself: nothing else can run while "
                       "hal_flash_safe_execute() has both cores' interrupts disabled, and 208 "
                       "such erases back to back otherwise outlast the 1000 ms watchdog");
            TEST_CHECK(strstr(body, "hal_wdt_feed") == NULL,
                       "the erase loop must NOT call hal_wdt_feed() directly -- an ungated feed "
                       "would let a genuinely wedged safety processor be held alive through an "
                       "update, which is the opposite of what the watchdog is for. It borrows "
                       "WHO owns the feed, never the POLICY of when one is allowed");
            TEST_CHECK(strstr(body, "update_erase_plan_chunk(") != NULL,
                       "the erase loop must walk the slot through the tested plan arithmetic "
                       "rather than recomputing offsets inline");
            free(body);
        }
    }

    free(text);
}

// Pins the restated TEST_SLOT_FLASH_SIZE above to the real definition, so this
// file cannot keep asserting "208 chunks" about a slot size that has changed.
static void test_slot_size_constant_matches_flash_layout(void)
{
    TEST_SECTION("flash_layout.h -- BOOTLOADER_SLOT_FLASH_SIZE still matches this test's constant");

    char *text = read_file_any(FLASH_LAYOUT_CANDIDATES,
                                sizeof(FLASH_LAYOUT_CANDIDATES) / sizeof(FLASH_LAYOUT_CANDIDATES[0]));
    TEST_CHECK(text != NULL, "flash_layout.h must be readable");
    if (!text) {
        return;
    }

    const char *def = strstr(text, "#define BOOTLOADER_SLOT_FLASH_SIZE");
    TEST_CHECK(def != NULL, "BOOTLOADER_SLOT_FLASH_SIZE must still be defined");
    if (def) {
        const char *eol = strchr(def, '\n');
        size_t line_len = eol ? (size_t)(eol - def) : strlen(def);
        char line[256];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1u;
        }
        memcpy(line, def, line_len);
        line[line_len] = '\0';

        TEST_CHECK(strstr(line, "0x000D0000") != NULL,
                   "the slot is still 0xD0000 bytes -- if this changed, the 208-chunk count in "
                   "this file and the ESP-side RELAY_ERASE_TIMEOUT_MS derivation both need "
                   "revisiting");
    }

    free(text);
}

void run_test_update_task_erase_plan(void)
{
    test_slot_is_exactly_208_chunks();
    test_walk_covers_slot_exactly_no_gap_no_overrun();
    test_index_past_end_is_refused();
    test_short_tail_is_covered_not_truncated();
    test_degenerate_arguments_refused();
    test_update_task_uses_4k_erase_chunks();
    test_erase_loop_feeds_through_the_shared_gate();
    test_slot_size_constant_matches_flash_layout();
}
