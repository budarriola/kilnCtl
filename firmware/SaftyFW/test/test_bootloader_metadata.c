// Host tests for firmware/SaftyFW/bootloader/{crc32,metadata}.c -- the
// pure, safety-critical logic that decides which application slot boots.
// No pico-sdk/FreeRTOS dependency, same discipline as test_safety_guards.c
// and test_link_frame.c.
#include <string.h>

#include "test_common.h"

#include "crc32.h"
#include "metadata.h"

static void fill_slot(bootloader_slot_meta_t *s, bootloader_slot_state_t state, uint32_t length,
                       uint32_t crc, const char *version, const char *commit, uint32_t epoch)
{
    memset(s, 0, sizeof(*s));
    s->state = state;
    s->length = length;
    s->crc32 = crc;
    memcpy(s->version, version, strlen(version) < sizeof(s->version) ? strlen(version)
                                                                       : sizeof(s->version));
    memcpy(s->build_commit, commit,
           strlen(commit) < sizeof(s->build_commit) ? strlen(commit) : sizeof(s->build_commit));
    s->build_epoch = epoch;
}

static void test_crc32(void)
{
    TEST_SECTION("bootloader_crc32 -- known test vector");
    // Canonical CRC-32 (zlib/IEEE 802.3) test vector: crc32("123456789") == 0xCBF43926.
    const uint8_t vec[] = "123456789";
    TEST_CHECK(bootloader_crc32(vec, 9) == 0xCBF43926u, "crc32(\"123456789\") == 0xCBF43926");
    TEST_CHECK(bootloader_crc32(NULL, 0) == 0x00000000u, "crc32 of empty input is 0");

    uint8_t single = 0;
    uint32_t crc_zero = bootloader_crc32(&single, 1);
    single = 1;
    uint32_t crc_one = bootloader_crc32(&single, 1);
    TEST_CHECK(crc_zero != crc_one, "single differing byte changes the CRC");
}

static void test_pack_unpack_roundtrip(void)
{
    TEST_SECTION("bootloader_metadata_pack/unpack -- roundtrip");

    bootloader_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
    meta.seq = 42;
    meta.active_slot = BOOTLOADER_SLOT_B;
    meta.boot_attempts = 2;
    fill_slot(&meta.slots[BOOTLOADER_SLOT_A], BOOTLOADER_SLOT_VALID, 123456, 0xDEADBEEFu,
              "1.2.3-a", "abcdef0123456789abcd", 1700000000u);
    fill_slot(&meta.slots[BOOTLOADER_SLOT_B], BOOTLOADER_SLOT_PENDING_VERIFY, 654321, 0xCAFEBABEu,
              "1.3.0-b", "0123456789abcdef0123", 1700000500u);

    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
    bootloader_metadata_pack(&meta, record);

    bootloader_metadata_t back;
    memset(&back, 0xAA, sizeof(back));
    bool ok = bootloader_metadata_unpack(record, &back);
    TEST_CHECK(ok, "well-formed record unpacks successfully");
    TEST_CHECK(back.format_version == meta.format_version, "format_version roundtrips");
    TEST_CHECK(back.seq == meta.seq, "seq roundtrips");
    TEST_CHECK(back.active_slot == meta.active_slot, "active_slot roundtrips");
    TEST_CHECK(back.boot_attempts == meta.boot_attempts, "boot_attempts roundtrips");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_A].state == BOOTLOADER_SLOT_VALID,
               "slot A state roundtrips");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_A].length == 123456, "slot A length roundtrips");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_A].crc32 == 0xDEADBEEFu, "slot A crc32 roundtrips");
    TEST_CHECK(memcmp(back.slots[BOOTLOADER_SLOT_A].version, "1.2.3-a", 7) == 0,
               "slot A version roundtrips");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_PENDING_VERIFY,
               "slot B state roundtrips");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_B].build_epoch == 1700000500u,
               "slot B build_epoch roundtrips");

    // Reserved signature/sig_required fields: all-zero in, all-zero out --
    // fill_slot() memsets the whole struct to 0 before setting the fields it
    // knows about, so this proves the reserved bytes roundtrip untouched
    // rather than being clobbered by pack/unpack.
    static const uint8_t zero_sig[64] = {0};
    TEST_CHECK(memcmp(back.slots[BOOTLOADER_SLOT_A].signature, zero_sig, 64) == 0,
               "slot A signature roundtrips as all-zero (no signature present)");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_A].sig_required == 0,
               "slot A sig_required roundtrips as 0");

    // Non-zero reserved fields must also roundtrip byte-for-byte -- this is
    // still just wire-format plumbing, not a claim that anything checks them.
    uint8_t sig_pattern[64];
    for (int i = 0; i < 64; i++) {
        sig_pattern[i] = (uint8_t)(i + 1);
    }
    memcpy(meta.slots[BOOTLOADER_SLOT_B].signature, sig_pattern, 64);
    meta.slots[BOOTLOADER_SLOT_B].sig_required = 1;
    bootloader_metadata_pack(&meta, record);
    ok = bootloader_metadata_unpack(record, &back);
    TEST_CHECK(ok, "record with non-zero reserved fields still unpacks");
    TEST_CHECK(memcmp(back.slots[BOOTLOADER_SLOT_B].signature, sig_pattern, 64) == 0,
               "slot B non-zero signature roundtrips byte-for-byte");
    TEST_CHECK(back.slots[BOOTLOADER_SLOT_B].sig_required == 1,
               "slot B non-zero sig_required roundtrips");
}

static void test_unpack_hostile(void)
{
    TEST_SECTION("bootloader_metadata_unpack -- hostile inputs");

    // Erased flash: all 0xFF. Must be rejected (wrong magic), not crash.
    uint8_t erased[BOOTLOADER_METADATA_RECORD_LEN];
    memset(erased, 0xFF, sizeof(erased));
    bootloader_metadata_t sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));
    bootloader_metadata_t out = sentinel;
    TEST_CHECK(!bootloader_metadata_unpack(erased, &out), "all-0xFF (erased) record rejected");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0,
               "*out untouched after rejecting erased record");

    // All zero: wrong magic.
    uint8_t zeroed[BOOTLOADER_METADATA_RECORD_LEN];
    memset(zeroed, 0x00, sizeof(zeroed));
    out = sentinel;
    TEST_CHECK(!bootloader_metadata_unpack(zeroed, &out), "all-zero record rejected (bad magic)");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0, "*out untouched after bad-magic reject");

    // Valid record, then flip one payload bit -- CRC must catch it (torn
    // write / flash corruption simulation).
    bootloader_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
    meta.seq = 7;
    meta.active_slot = BOOTLOADER_SLOT_A;
    fill_slot(&meta.slots[BOOTLOADER_SLOT_A], BOOTLOADER_SLOT_VALID, 100, 0x11111111u, "v", "c",
              0);
    fill_slot(&meta.slots[BOOTLOADER_SLOT_B], BOOTLOADER_SLOT_EMPTY, 0, 0, "", "", 0);
    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
    bootloader_metadata_pack(&meta, record);
    record[20] ^= 0x01u; // corrupt one payload byte, well before the trailing CRC field
    out = sentinel;
    TEST_CHECK(!bootloader_metadata_unpack(record, &out), "single-bit-flipped record rejected");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0,
               "*out untouched after CRC-mismatch reject");

    // Wrong format_version.
    bootloader_metadata_pack(&meta, record); // re-pack clean
    record[4] = 0xFF;
    record[5] = 0xFF; // format_version -> 0xFFFF, unrecognised
    out = sentinel;
    TEST_CHECK(!bootloader_metadata_unpack(record, &out), "unrecognised format_version rejected");

    TEST_CHECK(!bootloader_metadata_unpack(NULL, &out), "NULL in rejected");
    TEST_CHECK(!bootloader_metadata_unpack(record, NULL), "NULL out rejected");
}

static void test_find_latest(void)
{
    TEST_SECTION("bootloader_metadata_find_latest -- sector scan");

    uint8_t sector[BOOTLOADER_METADATA_FLASH_SIZE];
    memset(sector, 0xFF, sizeof(sector)); // fully erased sector

    bootloader_metadata_t out;
    size_t slot = bootloader_metadata_find_latest(sector, &out);
    TEST_CHECK(slot == BOOTLOADER_METADATA_NO_SLOT, "fully erased sector: no valid record");

    // Write three records at increasing seq, out of slot order, to prove
    // this is a seq-scan, not "last slot wins" or "first slot wins".
    bootloader_metadata_t m5, m9, m2;
    memset(&m5, 0, sizeof(m5));
    m5.format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
    m5.seq = 5;
    m5.active_slot = BOOTLOADER_SLOT_A;
    fill_slot(&m5.slots[BOOTLOADER_SLOT_A], BOOTLOADER_SLOT_VALID, 10, 1, "a", "a", 0);
    fill_slot(&m5.slots[BOOTLOADER_SLOT_B], BOOTLOADER_SLOT_EMPTY, 0, 0, "", "", 0);

    m9 = m5;
    m9.seq = 9;
    m2 = m5;
    m2.seq = 2;

    bootloader_metadata_pack(&m9, &sector[3 * BOOTLOADER_METADATA_RECORD_LEN]); // slot 3: seq 9
    bootloader_metadata_pack(&m2, &sector[0 * BOOTLOADER_METADATA_RECORD_LEN]); // slot 0: seq 2
    bootloader_metadata_pack(&m5, &sector[7 * BOOTLOADER_METADATA_RECORD_LEN]); // slot 7: seq 5

    slot = bootloader_metadata_find_latest(sector, &out);
    TEST_CHECK(slot == 3, "highest-seq record found regardless of slot position");
    TEST_CHECK(out.seq == 9, "returned metadata is the highest-seq record's contents");

    // Corrupt the winning record (slot 3) -- the next-highest (seq 5, slot 7)
    // must now win, proving corrupt records are skipped, not just deprioritised.
    sector[3 * BOOTLOADER_METADATA_RECORD_LEN + 20] ^= 0x01u;
    slot = bootloader_metadata_find_latest(sector, &out);
    TEST_CHECK(slot == 7, "corrupted highest-seq record is skipped in favour of the next-best");
    TEST_CHECK(out.seq == 5, "skips to the next-highest valid seq");
}

static void test_next_write_slot(void)
{
    TEST_SECTION("bootloader_metadata_next_write_slot / needs_erase");

    TEST_CHECK(bootloader_metadata_next_write_slot(BOOTLOADER_METADATA_NO_SLOT) == 0,
               "never-written sector: next write is slot 0");
    TEST_CHECK(!bootloader_metadata_next_write_needs_erase(BOOTLOADER_METADATA_NO_SLOT),
               "never-written sector: no erase needed for slot 0");

    TEST_CHECK(bootloader_metadata_next_write_slot(0) == 1, "slot 0 written -> next is slot 1");
    TEST_CHECK(!bootloader_metadata_next_write_needs_erase(0), "mid-sector: no erase needed");

    size_t last = BOOTLOADER_METADATA_SLOTS_PER_SECTOR - 1;
    TEST_CHECK(bootloader_metadata_next_write_slot(last) == 0,
               "last slot written -> wraps to slot 0");
    TEST_CHECK(bootloader_metadata_next_write_needs_erase(last),
               "wrap requires an erase before the slot-0 write");
}

static bootloader_metadata_t make_meta(uint8_t active, bootloader_slot_state_t a_state,
                                        bootloader_slot_state_t b_state, uint8_t boot_attempts)
{
    bootloader_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
    meta.seq = 1;
    meta.active_slot = active;
    meta.boot_attempts = boot_attempts;
    fill_slot(&meta.slots[BOOTLOADER_SLOT_A], a_state, 1000, 0x1u, "a", "a", 0);
    fill_slot(&meta.slots[BOOTLOADER_SLOT_B], b_state, 2000, 0x2u, "b", "b", 0);
    return meta;
}

static void test_decide_boot(void)
{
    TEST_SECTION("bootloader_decide_boot -- normal and fallback paths");

    // Healthy active slot, under the attempt limit -> boot it, increment attempts.
    bootloader_metadata_t m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_VALID,
                                         BOOTLOADER_SLOT_EMPTY, 0);
    bootloader_boot_decision_t d = bootloader_decide_boot(&m);
    TEST_CHECK(d.bootable, "VALID active slot under attempt limit is bootable");
    TEST_CHECK(d.chosen_slot == BOOTLOADER_SLOT_A, "chooses the active slot");
    TEST_CHECK(d.needs_metadata_update, "boot_attempts increment needs a metadata write");
    TEST_CHECK(d.updated_meta.boot_attempts == 1, "boot_attempts incremented from 0 to 1");
    TEST_CHECK(d.updated_meta.active_slot == BOOTLOADER_SLOT_A, "active_slot unchanged");

    // PENDING_VERIFY active slot boots too (docs/BOOTLOADER.md section 5:
    // the app needs the chance to confirm itself).
    m = make_meta(BOOTLOADER_SLOT_B, BOOTLOADER_SLOT_EMPTY, BOOTLOADER_SLOT_PENDING_VERIFY, 0);
    d = bootloader_decide_boot(&m);
    TEST_CHECK(d.bootable, "PENDING_VERIFY active slot is bootable");
    TEST_CHECK(d.chosen_slot == BOOTLOADER_SLOT_B, "chooses the PENDING_VERIFY slot");

    // Active slot exhausted its attempts, other slot VALID -> switch.
    m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_VALID, BOOTLOADER_SLOT_VALID,
                  BOOTLOADER_MAX_BOOT_ATTEMPTS);
    d = bootloader_decide_boot(&m);
    TEST_CHECK(d.bootable, "attempt-exhausted active slot falls back to a VALID other slot");
    TEST_CHECK(d.chosen_slot == BOOTLOADER_SLOT_B, "switches to slot B");
    TEST_CHECK(d.updated_meta.active_slot == BOOTLOADER_SLOT_B, "active_slot updated to B");
    TEST_CHECK(d.updated_meta.slots[BOOTLOADER_SLOT_A].state == BOOTLOADER_SLOT_BAD,
               "exhausted slot A marked BAD");
    TEST_CHECK(d.updated_meta.boot_attempts == 1, "boot_attempts reset to 1 for the new slot");

    // Active slot exhausted, other slot also not bootable -> recovery.
    m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_VALID, BOOTLOADER_SLOT_EMPTY,
                  BOOTLOADER_MAX_BOOT_ATTEMPTS);
    d = bootloader_decide_boot(&m);
    TEST_CHECK(!d.bootable, "both slots unusable after exhaustion -> not bootable (recovery)");
    TEST_CHECK(d.needs_metadata_update, "the BAD marking on slot A still needs to persist");
    TEST_CHECK(d.updated_meta.slots[BOOTLOADER_SLOT_A].state == BOOTLOADER_SLOT_BAD,
               "slot A recorded BAD even though nothing will boot");

    // Active slot EMPTY (first boot, nothing staged there), other slot VALID
    // -> falls back immediately, no attempt-limit reasoning needed.
    m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_EMPTY, BOOTLOADER_SLOT_VALID, 0);
    d = bootloader_decide_boot(&m);
    TEST_CHECK(d.bootable, "EMPTY active slot with a VALID sibling falls back immediately");
    TEST_CHECK(d.chosen_slot == BOOTLOADER_SLOT_B, "falls back to slot B");

    // Both slots unusable, no attempts exhausted -> not bootable.
    m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_EMPTY, BOOTLOADER_SLOT_BAD, 0);
    d = bootloader_decide_boot(&m);
    TEST_CHECK(!d.bootable, "both slots unusable from the start -> not bootable");

    // sig_required is dead weight: a VALID active slot with sig_required=1
    // and an all-zero signature must boot exactly as if sig_required were 0
    // -- this build has no verifier and must never start enforcing a check
    // it cannot perform (docs/BOOTLOADER.md section 2/6).
    m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_VALID, BOOTLOADER_SLOT_EMPTY, 0);
    m.slots[BOOTLOADER_SLOT_A].sig_required = 1;
    d = bootloader_decide_boot(&m);
    TEST_CHECK(d.bootable, "sig_required=1 with no signature does not block boot (unenforced)");
    TEST_CHECK(d.chosen_slot == BOOTLOADER_SLOT_A,
               "sig_required has no effect on which slot is chosen");
}

static void test_decide_after_crc_fail(void)
{
    TEST_SECTION("bootloader_decide_after_crc_fail");

    // Active slot's fresh CRC check failed; other slot is VALID -> fall back.
    bootloader_metadata_t m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_VALID,
                                         BOOTLOADER_SLOT_VALID, 1);
    bootloader_boot_decision_t d = bootloader_decide_after_crc_fail(&m, BOOTLOADER_SLOT_A);
    TEST_CHECK(d.bootable, "CRC failure on active slot falls back to a VALID other slot");
    TEST_CHECK(d.chosen_slot == BOOTLOADER_SLOT_B, "falls back to slot B");
    TEST_CHECK(d.updated_meta.slots[BOOTLOADER_SLOT_A].state == BOOTLOADER_SLOT_BAD,
               "CRC-failed slot A marked BAD");
    TEST_CHECK(d.updated_meta.active_slot == BOOTLOADER_SLOT_B, "active_slot switched to B");
    TEST_CHECK(d.updated_meta.boot_attempts == 1, "boot_attempts reset for the fallback slot");

    // Both slots fail (this one by CRC, the other genuinely unusable) -> recovery.
    m = make_meta(BOOTLOADER_SLOT_A, BOOTLOADER_SLOT_VALID, BOOTLOADER_SLOT_BAD, 1);
    d = bootloader_decide_after_crc_fail(&m, BOOTLOADER_SLOT_A);
    TEST_CHECK(!d.bootable, "CRC failure with no usable fallback -> not bootable (recovery)");
    TEST_CHECK(d.updated_meta.slots[BOOTLOADER_SLOT_A].state == BOOTLOADER_SLOT_BAD,
               "failed slot still recorded BAD even though nothing will boot");
}

void run_test_bootloader_metadata(void)
{
    test_crc32();
    test_pack_unpack_roundtrip();
    test_unpack_hostile();
    test_find_latest();
    test_next_write_slot();
    test_decide_boot();
    test_decide_after_crc_fail();
}
