// test_update_task_slot_linkage.c -- update_task_slot_linkage_check(), the
// vector-table plausibility check added 2026-09-20 (owner decision,
// docs/PICO_AUTO_UPDATE_PLAN.md) to catch a build/tooling mixup where an
// image linked for one flash slot's address gets written into the OTHER
// slot. That mixup CRCs correctly -- the bytes received are exactly the
// bytes sent -- so the existing UPDATE_END CRC check cannot see it; only a
// look at the two vector-table words (initial SP, reset vector) baked into
// the image at link time can. See update_task_slot_linkage.h's own header
// comment for the full rationale, and update_task.c's
// update_task_slot_linkage_plausible() for the on-target wrapper that reads
// those two words out of a mapped flash region and calls straight through
// to the pure function this file tests.
//
// FAILS CLOSED: a missing source file is a TEST FAILURE, never a skip.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

#include "tasks/update_task_slot_linkage.h"

// Real constants, restated here the same way test_update_task_erase_plan.c
// restates BOOTLOADER_SLOT_FLASH_SIZE (bootloader/flash_layout.h is not on
// the host-test include path). pico-sdk's hardware/regs/addressmap.h:
// SRAM_BASE 0x20000000, SRAM_END 0x20042000 -- together spanning exactly the
// RAM + SCRATCH_X + SCRATCH_Y regions bootloader/app_slot.ld.in's linker
// script carves out of SRAM (confirmed by reading that file).
#define TEST_SRAM_BASE 0x20000000u
#define TEST_SRAM_END  0x20042000u
#define TEST_XIP_BASE  0x10000000u

#define TEST_SLOT_A_OFFSET 0x00011000u
#define TEST_SLOT_B_OFFSET 0x000E1000u
#define TEST_SLOT_SIZE     0x000D0000u

// A plausible SP: top of RAM, the normal value for a fresh Cortex-M image's
// initial stack pointer.
#define PLAUSIBLE_SP (TEST_SRAM_BASE + 0x3F000u)

// A plausible image length -- large enough to contain the two
// vector-table words (8 bytes) the check is about; the exact value is
// otherwise irrelevant to every existing test case.
#define PLAUSIBLE_IMAGE_LENGTH 0x1000u

static uint32_t reset_vector_for(uint32_t xip_base, uint32_t slot_offset, uint32_t byte_into_slot)
{
    // Thumb-bit set, as every real Cortex-M code address must be.
    return xip_base + slot_offset + byte_into_slot + 1u;
}

// update_image_header_validate() (image_header.c) only rejects length == 0
// and length > max_length -- a 4-byte image passes that validation, and the
// on-target wrapper (update_task.c's update_task_slot_linkage_plausible())
// maps only those 4 bytes before reading the two vector-table words this
// check is about. Otherwise-perfectly-plausible sp/reset_vector values must
// still be rejected once image_length can't actually contain them.
static void test_image_too_short_for_vector_table_rejected(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- an image shorter than the vector table (4 bytes) is rejected");

    uint32_t rv = reset_vector_for(TEST_XIP_BASE, TEST_SLOT_A_OFFSET, 0x200u);

    bool four_bytes = update_task_slot_linkage_check(PLAUSIBLE_SP, rv, 4u, TEST_SLOT_A_OFFSET,
                                                      TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END,
                                                      TEST_XIP_BASE);
    TEST_CHECK(!four_bytes, "a 4-byte image (too short to hold both vector-table words) must be "
                             "rejected even though sp/reset_vector alone would otherwise pass");

    bool seven_bytes = update_task_slot_linkage_check(PLAUSIBLE_SP, rv, 7u, TEST_SLOT_A_OFFSET,
                                                       TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END,
                                                       TEST_XIP_BASE);
    TEST_CHECK(!seven_bytes, "a 7-byte image (one byte short of the full vector table) must also be rejected");

    bool eight_bytes = update_task_slot_linkage_check(PLAUSIBLE_SP, rv, 8u, TEST_SLOT_A_OFFSET,
                                                       TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END,
                                                       TEST_XIP_BASE);
    TEST_CHECK(eight_bytes, "an image of exactly 8 bytes (the full vector table, nothing more) must be accepted");
}

static void test_correct_slot_accepted(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- an image linked for its OWN target slot is accepted");

    uint32_t rv_a = reset_vector_for(TEST_XIP_BASE, TEST_SLOT_A_OFFSET, 0x200u);
    bool ok_a = update_task_slot_linkage_check(PLAUSIBLE_SP, rv_a, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET, TEST_SLOT_SIZE,
                                                TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);
    TEST_CHECK(ok_a, "a slot-A-linked reset vector targeting slot A must be accepted");

    uint32_t rv_b = reset_vector_for(TEST_XIP_BASE, TEST_SLOT_B_OFFSET, 0x200u);
    bool ok_b = update_task_slot_linkage_check(PLAUSIBLE_SP, rv_b, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_B_OFFSET, TEST_SLOT_SIZE,
                                                TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);
    TEST_CHECK(ok_b, "a slot-B-linked reset vector targeting slot B must be accepted");
}

// The actual defect class this check exists to catch: a slot-A image landed
// in slot B (the CRC over the received bytes is correct either way -- this
// is a linkage mismatch, not corruption).
static void test_slot_a_image_into_slot_b_rejected(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- a slot-A-linked image written into slot B is rejected");

    uint32_t rv_linked_for_a = reset_vector_for(TEST_XIP_BASE, TEST_SLOT_A_OFFSET, 0x200u);

    bool ok = update_task_slot_linkage_check(PLAUSIBLE_SP, rv_linked_for_a, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_B_OFFSET,
                                              TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);

    TEST_CHECK(!ok, "a reset vector inside slot A's window must be rejected when the TARGET slot is B "
                     "-- this is the exact wrong-slot-image signature the check exists to catch");
}

static void test_slot_b_image_into_slot_a_rejected(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- a slot-B-linked image written into slot A is rejected");

    uint32_t rv_linked_for_b = reset_vector_for(TEST_XIP_BASE, TEST_SLOT_B_OFFSET, 0x200u);

    bool ok = update_task_slot_linkage_check(PLAUSIBLE_SP, rv_linked_for_b, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                              TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);

    TEST_CHECK(!ok, "a reset vector inside slot B's window must be rejected when the TARGET slot is A");
}

static void test_garbage_sp_rejected(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- an SP outside SRAM is rejected");

    uint32_t rv = reset_vector_for(TEST_XIP_BASE, TEST_SLOT_A_OFFSET, 0x200u);

    bool below = update_task_slot_linkage_check(TEST_SRAM_BASE - 4u, rv, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                                 TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);
    TEST_CHECK(!below, "an SP one word below SRAM_BASE must be rejected");

    bool above = update_task_slot_linkage_check(TEST_SRAM_END + 4u, rv, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                                 TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);
    TEST_CHECK(!above, "an SP one word past SRAM_END must be rejected");

    bool zero = update_task_slot_linkage_check(0u, rv, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET, TEST_SLOT_SIZE,
                                                TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);
    TEST_CHECK(!zero, "a NULL/zero SP (an erased-flash or all-zero vector table) must be rejected");

    bool at_top = update_task_slot_linkage_check(TEST_SRAM_END, rv, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET, TEST_SLOT_SIZE,
                                                  TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);
    TEST_CHECK(at_top, "SP == SRAM_END (one past the last usable byte) is the normal top-of-stack "
                        "value and must be ACCEPTED, not rejected as an overrun");
}

static void test_reset_vector_without_thumb_bit_rejected(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- a reset vector without the Thumb bit is rejected");

    // Same address as an accepted case above, but with bit 0 cleared --
    // otherwise perfectly in-range for slot A.
    uint32_t rv_no_thumb = TEST_XIP_BASE + TEST_SLOT_A_OFFSET + 0x200u; // even -- Thumb bit clear

    bool ok = update_task_slot_linkage_check(PLAUSIBLE_SP, rv_no_thumb, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                              TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);

    TEST_CHECK(!ok, "a reset vector with bit 0 clear cannot be a valid Cortex-M code address on a "
                     "core with no ARM mode, and must be rejected regardless of which window it "
                     "otherwise falls in");
}

static void test_reset_vector_outside_either_slot_rejected(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- a reset vector outside any slot window is rejected");

    // Well past both slots -- e.g. pointing into the metadata or config
    // regions instead of application code.
    uint32_t rv_far_away = TEST_XIP_BASE + 0x00010000u + 1u; // metadata region, not a slot

    bool ok = update_task_slot_linkage_check(PLAUSIBLE_SP, rv_far_away, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                              TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END, TEST_XIP_BASE);

    TEST_CHECK(!ok, "a reset vector outside the target slot's own window entirely must be rejected");
}

static void test_boundary_reset_vectors(void)
{
    TEST_SECTION("update_task_slot_linkage_check -- slot window is [start, start+size), half-open");

    uint32_t window_start = TEST_XIP_BASE + TEST_SLOT_A_OFFSET;
    uint32_t window_end = window_start + TEST_SLOT_SIZE;

    // First valid instruction address in the slot (Thumb bit set).
    bool first_ok = update_task_slot_linkage_check(PLAUSIBLE_SP, window_start + 1u, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                                    TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END,
                                                    TEST_XIP_BASE);
    TEST_CHECK(first_ok, "a reset vector at the very start of the slot's window must be accepted");

    // One past the end of the slot (Thumb bit set) -- must be rejected: this
    // address belongs to whatever follows the slot, not the slot itself.
    bool past_end = update_task_slot_linkage_check(PLAUSIBLE_SP, window_end + 1u, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                                    TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END,
                                                    TEST_XIP_BASE);
    TEST_CHECK(!past_end, "a reset vector at or past the slot's end must be rejected -- the window "
                           "is half-open, [start, start+size)");

    // Exactly at window_end (Thumb bit forced via |1u rather than +1u, so
    // this pins the boundary address itself rather than one past it) --
    // must be rejected. This is the address that distinguishes a half-open
    // upper bound (`< window_end`, correct) from an inclusive one
    // (`<= window_end`, off-by-one): window_end belongs to whatever follows
    // the slot, not the slot itself.
    bool at_end_thumb = update_task_slot_linkage_check(PLAUSIBLE_SP, window_end | 1u, PLAUSIBLE_IMAGE_LENGTH, TEST_SLOT_A_OFFSET,
                                                        TEST_SLOT_SIZE, TEST_SRAM_BASE, TEST_SRAM_END,
                                                        TEST_XIP_BASE);
    TEST_CHECK(!at_end_thumb, "a reset vector exactly at the slot's end (window_end | 1u, Thumb bit "
                              "set) must be rejected, pinning >= vs > at the upper boundary");
}

void run_test_update_task_slot_linkage(void)
{
    test_image_too_short_for_vector_table_rejected();
    test_correct_slot_accepted();
    test_slot_a_image_into_slot_b_rejected();
    test_slot_b_image_into_slot_a_rejected();
    test_garbage_sp_rejected();
    test_reset_vector_without_thumb_bit_rejected();
    test_reset_vector_outside_either_slot_rejected();
    test_boundary_reset_vectors();
}
