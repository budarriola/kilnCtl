// test_update_task_flash_guard.c -- update_task_flash_guard_overlaps() and
// update_task_flash_guard_running_image_extent_valid(), the pure overlap
// arithmetic behind the hard refusal to erase or program any flash region
// that overlaps the RUNNING image's own extent (2026-09-21 triage: a flat,
// bootloader-less bench image loaded at XIP_BASE overlaps the metadata
// sector and part of slot A; update_task_persist_metadata() programming the
// metadata sector on that board corrupted its own running code). See
// update_task_flash_guard.h's own header comment for the full rationale.
//
// FAILS CLOSED: a missing source file is a TEST FAILURE, never a skip.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

#include "tasks/update_task_flash_guard.h"

// Real constants, restated here the same way test_update_task_slot_linkage.c
// restates them (bootloader/flash_layout.h is not on the host-test include
// path).
#define TEST_METADATA_OFFSET 0x00010000u
#define TEST_METADATA_SIZE   0x00001000u
#define TEST_SLOT_A_OFFSET   0x00011000u
#define TEST_SLOT_B_OFFSET   0x000E1000u
#define TEST_SLOT_SIZE       0x000D0000u

// The flat, bootloader-less bench image from the triage report: loaded at
// flash offset 0, size 0x1D204 -- overlaps the metadata sector entirely and
// the first 0xC204 bytes of slot A.
#define FLAT_IMAGE_START 0x00000000u
#define FLAT_IMAGE_END   0x0001D204u

// A normal, correctly slot-linked running image: e.g. currently running out
// of slot A, occupying a modest prefix of it.
#define SLOT_LINKED_START (TEST_SLOT_A_OFFSET)
#define SLOT_LINKED_END   (TEST_SLOT_A_OFFSET + 0x8000u)

static void test_extent_validity(void)
{
    TEST_SECTION("update_task_flash_guard_running_image_extent_valid -- well-formed vs malformed extents");

    TEST_CHECK(update_task_flash_guard_running_image_extent_valid(SLOT_LINKED_START, SLOT_LINKED_END),
               "a normal start < end extent must be valid");
    TEST_CHECK(!update_task_flash_guard_running_image_extent_valid(0x1000u, 0x1000u),
               "an empty extent (end == start) must be invalid");
    TEST_CHECK(!update_task_flash_guard_running_image_extent_valid(0x2000u, 0x1000u),
               "a reversed extent (end < start) must be invalid");
}

// The actual defect class: a flat/bootloader-less running image whose extent
// overlaps the metadata sector must refuse a metadata program/erase there.
static void test_flat_image_overlaps_metadata_sector(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- the flat bench image overlaps the metadata sector");

    bool overlap = update_task_flash_guard_overlaps(TEST_METADATA_OFFSET, TEST_METADATA_SIZE,
                                                      FLAT_IMAGE_START, FLAT_IMAGE_END);
    TEST_CHECK(overlap, "a flat image spanning [0x0, 0x1D204) must be reported as overlapping the "
                         "metadata sector at [0x10000, 0x11000) -- this is the exact hazard that "
                         "corrupted the bench board's own running code");
}

static void test_flat_image_overlaps_slot_a_prefix(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- the flat bench image overlaps slot A's first bytes");

    bool overlap = update_task_flash_guard_overlaps(TEST_SLOT_A_OFFSET, TEST_SLOT_SIZE,
                                                      FLAT_IMAGE_START, FLAT_IMAGE_END);
    TEST_CHECK(overlap, "the flat image's tail (up to 0x1D204) reaches 0xC204 bytes into slot A "
                         "(which starts at 0x11000) -- erasing slot A while this image runs must "
                         "also be refused");
}

static void test_flat_image_does_not_overlap_slot_b(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- the flat bench image does NOT reach slot B");

    bool overlap = update_task_flash_guard_overlaps(TEST_SLOT_B_OFFSET, TEST_SLOT_SIZE,
                                                      FLAT_IMAGE_START, FLAT_IMAGE_END);
    TEST_CHECK(!overlap, "slot B starts at 0xE1000, well past the flat image's end at 0x1D204 -- "
                          "erasing slot B alone is genuinely harmless and must not be refused "
                          "(this is exactly why the bench board's ERASING state was reached before "
                          "the hang: slot B's own erase succeeded)");
}

// Normal two-slot path: a properly slot-linked running image (e.g. active in
// slot A) must not be flagged as overlapping the metadata sector or the
// OTHER slot it is not occupying.
static void test_slot_linked_image_does_not_overlap_metadata(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- a normal slot-A-linked running image never reaches metadata");

    bool overlap = update_task_flash_guard_overlaps(TEST_METADATA_OFFSET, TEST_METADATA_SIZE,
                                                      SLOT_LINKED_START, SLOT_LINKED_END);
    TEST_CHECK(!overlap, "a slot-A-linked image (starting at 0x11000) can never reach back to the "
                          "metadata sector at 0x10000..0x11000 -- the ordinary two-slot update path "
                          "must be unaffected by this guard");
}

static void test_slot_linked_image_does_not_overlap_other_slot(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- a slot-A-linked running image does not overlap slot B");

    bool overlap = update_task_flash_guard_overlaps(TEST_SLOT_B_OFFSET, TEST_SLOT_SIZE,
                                                      SLOT_LINKED_START, SLOT_LINKED_END);
    TEST_CHECK(!overlap, "erasing the INACTIVE slot (B) while running from slot A is the whole point "
                          "of the two-slot scheme and must never be refused by this guard");
}

// Boundary: half-open ranges on both sides.
static void test_boundary_touching_ranges_do_not_overlap(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- half-open ranges that only touch at a boundary do not overlap");

    // Running image ends exactly where the region starts.
    bool touch_at_start = update_task_flash_guard_overlaps(0x2000u, 0x1000u, 0x1000u, 0x2000u);
    TEST_CHECK(!touch_at_start, "an image extent [0x1000,0x2000) and a region [0x2000,0x3000) share "
                                 "no byte -- must not overlap");

    // Running image starts exactly where the region ends.
    bool touch_at_end = update_task_flash_guard_overlaps(0x1000u, 0x1000u, 0x2000u, 0x3000u);
    TEST_CHECK(!touch_at_end, "a region [0x1000,0x2000) and an image extent [0x2000,0x3000) share no "
                               "byte -- must not overlap");

    // One byte of overlap at each boundary.
    bool one_byte_low = update_task_flash_guard_overlaps(0x1FFFu, 0x1u, 0x1000u, 0x2000u);
    TEST_CHECK(one_byte_low, "a 1-byte region at 0x1FFF is the last byte of the image extent "
                              "[0x1000,0x2000) -- must overlap");

    bool one_byte_high = update_task_flash_guard_overlaps(0x1000u, 0x1u, 0x1000u, 0x2000u);
    TEST_CHECK(one_byte_high, "a 1-byte region at 0x1000 is the first byte of the image extent "
                               "[0x1000,0x2000) -- must overlap");
}

static void test_zero_size_region_never_overlaps(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- a zero-size region never overlaps anything");

    bool overlap = update_task_flash_guard_overlaps(0x1500u, 0u, FLAT_IMAGE_START, FLAT_IMAGE_END);
    TEST_CHECK(!overlap, "a zero-size region touches no bytes and must never be reported as overlapping, "
                          "even one squarely inside the flat image's own extent");
}

// The malformed-extent fail-closed behaviour: update_task_flash_guard_
// overlaps() itself must also refuse (report overlap) if ever handed a
// malformed extent directly, as defence in depth against a caller that
// skips update_task_flash_guard_running_image_extent_valid().
static void test_malformed_extent_fails_closed(void)
{
    TEST_SECTION("update_task_flash_guard_overlaps -- a malformed running-image extent fails closed (reports overlap)");

    bool empty_extent = update_task_flash_guard_overlaps(TEST_SLOT_B_OFFSET, TEST_SLOT_SIZE, 0x5000u, 0x5000u);
    TEST_CHECK(empty_extent, "an empty extent (end == start) must never be read as \"no overlap\" -- "
                              "a guard that cannot determine its own extent must refuse, not proceed");

    bool reversed_extent = update_task_flash_guard_overlaps(TEST_SLOT_B_OFFSET, TEST_SLOT_SIZE, 0x9000u, 0x1000u);
    TEST_CHECK(reversed_extent, "a reversed extent (end < start) must also fail closed as \"overlap\"");
}

void run_test_update_task_flash_guard(void)
{
    test_extent_validity();
    test_flat_image_overlaps_metadata_sector();
    test_flat_image_overlaps_slot_a_prefix();
    test_flat_image_does_not_overlap_slot_b();
    test_slot_linked_image_does_not_overlap_metadata();
    test_slot_linked_image_does_not_overlap_other_slot();
    test_boundary_touching_ranges_do_not_overlap();
    test_zero_size_region_never_overlaps();
    test_malformed_extent_fails_closed();
}
