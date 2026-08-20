// Host tests for firmware/SaftyFW/src/config_store.c -- the pure config
// record pack/unpack/CRC/versioning logic and the ARMED-write-refusal
// decision. No pico-sdk/FreeRTOS dependency, same discipline as
// test_bootloader_metadata.c (config_store.c mirrors bootloader/metadata.c's
// pattern almost exactly). config_store_flash.c (the real flash I/O and
// relay_owner ARMED check) is NOT covered here -- it needs a real RP2040,
// same reason update_task.c has no host test file.
#include <string.h>

#include "test_common.h"

#include "config_store.h"

static void test_pack_unpack_roundtrip(void)
{
    TEST_SECTION("config_store_pack/unpack -- roundtrip");

    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    rec.seq = 42;
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;
    for (size_t i = 0; i < sizeof(rec.reserved); i++) {
        rec.reserved[i] = (uint8_t)(i + 1);
    }

    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);

    config_store_record_t back;
    memset(&back, 0xAA, sizeof(back));
    bool ok = config_store_unpack(record, &back);
    TEST_CHECK(ok, "well-formed record unpacks successfully");
    TEST_CHECK(back.format_version == rec.format_version, "format_version roundtrips");
    TEST_CHECK(back.seq == rec.seq, "seq roundtrips");
    TEST_CHECK(back.tc_type == rec.tc_type, "tc_type roundtrips");
    TEST_CHECK(back.calibration_missing == false, "calibration_missing (false) roundtrips");
    TEST_CHECK(memcmp(back.reserved, rec.reserved, sizeof(rec.reserved)) == 0,
               "reserved bytes roundtrip byte-for-byte");

    // calibration_missing = true must roundtrip too, not just its zero value.
    rec.calibration_missing = true;
    config_store_pack(&rec, record);
    ok = config_store_unpack(record, &back);
    TEST_CHECK(ok, "record with calibration_missing=true unpacks");
    TEST_CHECK(back.calibration_missing == true, "calibration_missing (true) roundtrips");
}

static void test_unpack_hostile(void)
{
    TEST_SECTION("config_store_unpack -- hostile inputs");

    // Erased flash: all 0xFF. Must be rejected (wrong magic), not crash.
    uint8_t erased[CONFIG_STORE_RECORD_LEN];
    memset(erased, 0xFF, sizeof(erased));
    config_store_record_t sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));
    config_store_record_t out = sentinel;
    TEST_CHECK(!config_store_unpack(erased, &out), "all-0xFF (erased) record rejected");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0,
               "*out untouched after rejecting erased record");

    // All zero: wrong magic.
    uint8_t zeroed[CONFIG_STORE_RECORD_LEN];
    memset(zeroed, 0x00, sizeof(zeroed));
    out = sentinel;
    TEST_CHECK(!config_store_unpack(zeroed, &out), "all-zero record rejected (bad magic)");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0, "*out untouched after bad-magic reject");

    // Valid record, then flip one payload bit -- CRC must catch it (torn
    // write / flash corruption simulation).
    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    rec.seq = 7;
    rec.tc_type = 0x03u;
    rec.calibration_missing = true;
    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);
    record[20] ^= 0x01u; // corrupt one payload byte, well before the trailing CRC field
    out = sentinel;
    TEST_CHECK(!config_store_unpack(record, &out), "single-bit-flipped record rejected");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0,
               "*out untouched after CRC-mismatch reject");

    // Wrong format_version.
    config_store_pack(&rec, record); // re-pack clean
    record[4] = 0xFF;
    record[5] = 0xFF; // format_version -> 0xFFFF, unrecognised
    out = sentinel;
    TEST_CHECK(!config_store_unpack(record, &out), "unrecognised format_version rejected");

    TEST_CHECK(!config_store_unpack(NULL, &out), "NULL in rejected");
    TEST_CHECK(!config_store_unpack(record, NULL), "NULL out rejected");
}

static void test_default(void)
{
    TEST_SECTION("config_store_default -- safe defaults");

    config_store_record_t rec;
    memset(&rec, 0xAA, sizeof(rec));
    config_store_default(&rec);
    TEST_CHECK(rec.format_version == CONFIG_STORE_FORMAT_VERSION,
               "default format_version is current");
    TEST_CHECK(rec.seq == 0, "default seq is 0");
    TEST_CHECK(rec.tc_type == CONFIG_STORE_DEFAULT_TC_TYPE, "default tc_type is K");
    TEST_CHECK(rec.calibration_missing == true, "default calibration_missing is true");
}

static void test_find_latest(void)
{
    TEST_SECTION("config_store_find_latest -- sector scan");

    uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE];
    memset(sector, 0xFF, sizeof(sector)); // fully erased sector

    config_store_record_t out;
    size_t slot = config_store_find_latest(sector, &out);
    TEST_CHECK(slot == CONFIG_STORE_NO_SLOT, "fully erased sector: no valid record");

    // Write three records at increasing seq, out of slot order, to prove
    // this is a seq-scan, not "last slot wins" or "first slot wins".
    config_store_record_t r5, r9, r2;
    memset(&r5, 0, sizeof(r5));
    r5.format_version = CONFIG_STORE_FORMAT_VERSION;
    r5.seq = 5;
    r5.tc_type = 0x03u;
    r5.calibration_missing = true;

    r9 = r5;
    r9.seq = 9;
    r9.tc_type = 0x07u;
    r2 = r5;
    r2.seq = 2;

    config_store_pack(&r9, &sector[3 * CONFIG_STORE_RECORD_LEN]); // slot 3: seq 9
    config_store_pack(&r2, &sector[0 * CONFIG_STORE_RECORD_LEN]); // slot 0: seq 2
    config_store_pack(&r5, &sector[7 * CONFIG_STORE_RECORD_LEN]); // slot 7: seq 5

    slot = config_store_find_latest(sector, &out);
    TEST_CHECK(slot == 3, "highest-seq record found regardless of slot position");
    TEST_CHECK(out.seq == 9, "returned record is the highest-seq record's contents");
    TEST_CHECK(out.tc_type == 0x07u, "returned record's tc_type matches the winning record");

    // Corrupt the winning record (slot 3) -- the next-highest (seq 5, slot 7)
    // must now win, proving corrupt records are skipped, not just deprioritised.
    sector[3 * CONFIG_STORE_RECORD_LEN + 20] ^= 0x01u;
    slot = config_store_find_latest(sector, &out);
    TEST_CHECK(slot == 7, "corrupted highest-seq record is skipped in favour of the next-best");
    TEST_CHECK(out.seq == 5, "skips to the next-highest valid seq");

    TEST_CHECK(config_store_find_latest(NULL, &out) == CONFIG_STORE_NO_SLOT, "NULL sector rejected");
    TEST_CHECK(config_store_find_latest(sector, NULL) == CONFIG_STORE_NO_SLOT, "NULL out rejected");
}

static void test_next_write_slot(void)
{
    TEST_SECTION("config_store_next_write_slot / needs_erase");

    TEST_CHECK(config_store_next_write_slot(CONFIG_STORE_NO_SLOT) == 0,
               "never-written sector: next write is slot 0");
    TEST_CHECK(!config_store_next_write_needs_erase(CONFIG_STORE_NO_SLOT),
               "never-written sector: no erase needed for slot 0");

    TEST_CHECK(config_store_next_write_slot(0) == 1, "slot 0 written -> next is slot 1");
    TEST_CHECK(!config_store_next_write_needs_erase(0), "mid-sector: no erase needed");

    size_t last = CONFIG_STORE_SLOTS_PER_SECTOR - 1;
    TEST_CHECK(config_store_next_write_slot(last) == 0, "last slot written -> wraps to slot 0");
    TEST_CHECK(config_store_next_write_needs_erase(last),
               "wrap requires an erase before the slot-0 write");
}

static void test_decide_write(void)
{
    TEST_SECTION("config_store_decide_write -- ARMED refusal");

    TEST_CHECK(config_store_decide_write(false) == CONFIG_STORE_WRITE_OK,
               "not ARMED: write allowed");
    TEST_CHECK(config_store_decide_write(true) == CONFIG_STORE_WRITE_REFUSED_ARMED,
               "ARMED: write refused");

    const char *ok_reason = config_store_write_decision_reason(CONFIG_STORE_WRITE_OK);
    const char *refused_reason =
        config_store_write_decision_reason(CONFIG_STORE_WRITE_REFUSED_ARMED);
    TEST_CHECK(ok_reason != NULL && strlen(ok_reason) > 0, "OK reason is a non-empty string");
    TEST_CHECK(refused_reason != NULL && strlen(refused_reason) > 0,
               "refused reason is a non-empty string");
    TEST_CHECK(strcmp(ok_reason, refused_reason) != 0, "OK and refused reasons differ");
}

void run_test_config_store(void)
{
    test_pack_unpack_roundtrip();
    test_unpack_hostile();
    test_default();
    test_find_latest();
    test_next_write_slot();
    test_decide_write();
}
