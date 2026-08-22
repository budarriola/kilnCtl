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
    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        rec.ct_cal[i].calibrated = (i % 2u) == 0u;
        rec.ct_cal[i].gain = 1.0f + (float)i * 0.25f;
        rec.ct_cal[i].offset = -0.5f + (float)i * 0.1f;
    }
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
    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        TEST_CHECK(back.ct_cal[i].calibrated == rec.ct_cal[i].calibrated,
                   "ct_cal[i].calibrated roundtrips");
        TEST_CHECK(back.ct_cal[i].gain == rec.ct_cal[i].gain, "ct_cal[i].gain roundtrips");
        TEST_CHECK(back.ct_cal[i].offset == rec.ct_cal[i].offset, "ct_cal[i].offset roundtrips");
    }
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
    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        TEST_CHECK(rec.ct_cal[i].calibrated == false,
                   "default ct_cal[i].calibrated is false -- uncalibrated, not zero-gain");
    }
}

static void test_ct_cal_defaults_on_blank(void)
{
    TEST_SECTION("ct_cal -- defaults-on-blank/corrupt sector");

    // A fully erased sector: find_latest() finds nothing, and the caller
    // (config_store_flash.c's read_latest_or_default(), mirrored here) must
    // fall back to config_store_default() -- every channel uncalibrated,
    // never a crash and never a plausible-looking wrong gain/offset.
    uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE];
    memset(sector, 0xFF, sizeof(sector));

    config_store_record_t out;
    size_t slot = config_store_find_latest(sector, &out);
    TEST_CHECK(slot == CONFIG_STORE_NO_SLOT, "erased sector: no valid record found");

    config_store_record_t fallback;
    config_store_default(&fallback);
    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        TEST_CHECK(fallback.ct_cal[i].calibrated == false,
                   "erased-sector fallback: channel uncalibrated");
    }

    // A record written before this field existed: config_store_pack() with
    // rec.ct_cal left zero-initialized (memset 0, exactly what every SET_
    // CONFIG-authored record before this feature landed would have produced
    // in the byte range this field now claims). Must unpack as calibrated ==
    // false, not as a CRC failure and not as a stray "calibrated" reading.
    config_store_record_t old_rec;
    memset(&old_rec, 0, sizeof(old_rec));
    old_rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    old_rec.seq = 3;
    old_rec.tc_type = 0x03u;
    old_rec.calibration_missing = true;
    // old_rec.ct_cal left all-zero -- exactly what a pre-this-pass record's
    // corresponding bytes held (config_store_default()'s memset).
    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&old_rec, packed);

    config_store_record_t unpacked;
    bool ok = config_store_unpack(packed, &unpacked);
    TEST_CHECK(ok, "pre-ct_cal-shaped record still unpacks (no format_version bump was needed)");
    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        TEST_CHECK(unpacked.ct_cal[i].calibrated == false,
                   "pre-ct_cal record: every channel reads back uncalibrated, not corrupted");
    }
}

static void test_ct_cal_corrupt_or_unknown_version(void)
{
    TEST_SECTION("ct_cal -- corrupt or unknown-version record never surfaces stale calibration");

    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    rec.seq = 11;
    rec.tc_type = 0x03u;
    rec.calibration_missing = false;
    rec.ct_cal[0].calibrated = true;
    rec.ct_cal[0].gain = 2.5f;
    rec.ct_cal[0].offset = 0.75f;

    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);

    // Corrupt one payload byte inside the ct_cal region -- the record must
    // be rejected WHOLESALE (CRC covers everything before it), not partially
    // trusted with a flipped gain.
    record[18] ^= 0x01u; // inside channel 0's ct_cal bytes (offset 16..24)
    config_store_record_t sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));
    config_store_record_t out = sentinel;
    TEST_CHECK(!config_store_unpack(record, &out),
               "a corrupted ct_cal byte fails the whole record's CRC check");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0,
               "on CRC failure, *out is left completely untouched (caller must fall back to config_store_default())");

    // Unknown format_version -- same wholesale-reject rule.
    config_store_pack(&rec, record); // re-pack clean
    record[4] = 0xFF;
    record[5] = 0xFF; // format_version -> unrecognised
    memset(&out, 0xAA, sizeof(out));
    TEST_CHECK(!config_store_unpack(record, &out),
               "unrecognised format_version rejects the whole record, ct_cal included");
}

static void test_ct_cal_round_trip_and_independence(void)
{
    TEST_SECTION("ct_cal -- round trip through pack/unpack, per-channel independence");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.ct_cal[0].calibrated = true;
    rec.ct_cal[0].gain = 1.02f;
    rec.ct_cal[0].offset = -0.01f;
    // Channel 1 left uncalibrated (config_store_default()'s shape).
    rec.ct_cal[2].calibrated = true;
    rec.ct_cal[2].gain = 0.98f;
    rec.ct_cal[2].offset = 0.05f;

    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);

    config_store_record_t back;
    TEST_CHECK(config_store_unpack(record, &back), "mixed calibrated/uncalibrated record unpacks");

    TEST_CHECK(back.ct_cal[0].calibrated == true, "channel 0: calibrated round-trips true");
    TEST_CHECK(back.ct_cal[0].gain == 1.02f, "channel 0: gain round-trips exactly");
    TEST_CHECK(back.ct_cal[0].offset == -0.01f, "channel 0: offset round-trips exactly");

    TEST_CHECK(back.ct_cal[1].calibrated == false,
               "channel 1: still uncalibrated -- setting channel 0/2 did not leak into it");

    TEST_CHECK(back.ct_cal[2].calibrated == true, "channel 2: calibrated round-trips true");
    TEST_CHECK(back.ct_cal[2].gain == 0.98f, "channel 2: its own gain, not channel 0's");
    TEST_CHECK(back.ct_cal[2].offset == 0.05f, "channel 2: its own offset, not channel 0's");

    // Now flip channel 1 on and channel 0 off, proving independence holds in
    // both directions, not just "channel 0 happened to be first."
    rec.ct_cal[0].calibrated = false;
    rec.ct_cal[1].calibrated = true;
    rec.ct_cal[1].gain = 3.0f;
    rec.ct_cal[1].offset = 1.0f;
    config_store_pack(&rec, record);
    TEST_CHECK(config_store_unpack(record, &back), "re-packed record unpacks");
    TEST_CHECK(back.ct_cal[0].calibrated == false, "channel 0 now uncalibrated as set");
    TEST_CHECK(back.ct_cal[1].calibrated == true, "channel 1 now calibrated as set");
    TEST_CHECK(back.ct_cal[1].gain == 3.0f, "channel 1's own new gain");
    TEST_CHECK(back.ct_cal[2].calibrated == true,
               "channel 2 UNCHANGED by channel 0/1's flip -- true per-channel independence");
    TEST_CHECK(back.ct_cal[2].gain == 0.98f, "channel 2's gain still its own original value");
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

static void test_record_crc(void)
{
    TEST_SECTION("config_store_record_crc -- matches the packed record's trailing CRC");

    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    rec.seq = 123;
    rec.tc_type = 0x05u; // MAX31856_TC_TYPE_R
    rec.calibration_missing = false;

    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, packed);

    // The CRC is the last 4 bytes before the trailing pad (config_store.c's
    // REC_OFF_CRC = 248), little-endian -- read directly rather than
    // duplicating the offset constant here, since this test only needs to
    // prove config_store_record_crc() agrees with what pack() actually wrote.
    uint32_t expected = (uint32_t)packed[248] | ((uint32_t)packed[249] << 8) |
                         ((uint32_t)packed[250] << 16) | ((uint32_t)packed[251] << 24);

    uint32_t got = config_store_record_crc(&rec);
    TEST_CHECK(got == expected, "config_store_record_crc() matches config_store_pack()'s trailing CRC");

    // Changing any field the CRC covers must change the result -- otherwise
    // it isn't actually protecting anything.
    config_store_record_t rec2 = rec;
    rec2.tc_type = 0x06u;
    TEST_CHECK(config_store_record_crc(&rec2) != got,
               "a changed field changes the computed CRC");
}

static void test_confirm_crc_ok(void)
{
    TEST_SECTION("config_store_confirm_crc_ok -- update_task's config_crc_ok gate");

    // "config store never loaded": config_store_get_config_version() returns
    // 0 both before config_store_boot_load() has run and after it runs but
    // finds nothing valid (blank/corrupt sector) -- config_store_flash.c's
    // getter contract. Either way, version 0 must gate CLOSED.
    TEST_CHECK(!config_store_confirm_crc_ok(0u),
               "version 0 (never loaded / nothing valid found): gate stays closed");

    // "CRC mismatched" in practice can never surface as a version -- a
    // record whose CRC does not validate is rejected wholesale by
    // config_store_unpack() (test_unpack_hostile() above) and never becomes
    // the cached record at all, so config_store_get_config_version() falls
    // back to the default's seq (0), the same "never loaded" signal above.
    // That fallback IS the mismatch case reads as "stay closed" -- covered
    // by the version==0 check; there is no separate non-zero "mismatched"
    // version to test, by construction of config_store_unpack()'s own
    // wholesale-reject rule.

    // "config CRC matches": any genuinely written, CRC-verified record's
    // version is >= 1 (config_store_write() always assigns current+1,
    // starting from the default's 0) -- must gate OPEN.
    TEST_CHECK(config_store_confirm_crc_ok(1u),
               "version 1 (first real write, CRC-verified): gate can open");
    TEST_CHECK(config_store_confirm_crc_ok(255u),
               "any other non-zero version: gate can open");
}

static void test_flash_rc_reason(void)
{
    TEST_SECTION("config_store_flash_rc_reason -- flash_safe_execute() failure surfacing");

    const char *ok = config_store_flash_rc_reason(CONFIG_STORE_FLASH_RC_OK);
    const char *timeout = config_store_flash_rc_reason(CONFIG_STORE_FLASH_RC_TIMEOUT);
    const char *not_permitted =
        config_store_flash_rc_reason(CONFIG_STORE_FLASH_RC_NOT_PERMITTED);
    const char *insufficient_resources =
        config_store_flash_rc_reason(CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES);
    const char *unrecognised = config_store_flash_rc_reason(-999);

    TEST_CHECK(ok != NULL && strcmp(ok, "ok") == 0, "PICO_OK reason is exactly \"ok\"");
    TEST_CHECK(timeout != NULL && strlen(timeout) > 0, "TIMEOUT reason is non-empty");
    TEST_CHECK(not_permitted != NULL && strlen(not_permitted) > 0,
               "NOT_PERMITTED reason is non-empty");
    TEST_CHECK(insufficient_resources != NULL && strlen(insufficient_resources) > 0,
               "INSUFFICIENT_RESOURCES reason is non-empty");
    TEST_CHECK(unrecognised != NULL && strlen(unrecognised) > 0,
               "unrecognised rc still returns a non-NULL, non-empty reason");

    // Every failure reason must be distinguishable from every other one --
    // this is the entire point of config_store_flash_rc_reason() existing
    // instead of one generic "flash write failed" string: a caller (or a
    // bench log) must be able to tell a transient timeout apart from a
    // firmware init-order bug (NOT_PERMITTED) apart from resource exhaustion.
    TEST_CHECK(strcmp(timeout, not_permitted) != 0, "TIMEOUT reason differs from NOT_PERMITTED");
    TEST_CHECK(strcmp(timeout, insufficient_resources) != 0,
               "TIMEOUT reason differs from INSUFFICIENT_RESOURCES");
    TEST_CHECK(strcmp(not_permitted, insufficient_resources) != 0,
               "NOT_PERMITTED reason differs from INSUFFICIENT_RESOURCES");
    TEST_CHECK(strcmp(timeout, unrecognised) != 0, "TIMEOUT reason differs from unrecognised-rc reason");
    TEST_CHECK(strcmp(ok, timeout) != 0, "ok reason differs from a failure reason");
}

void run_test_config_store(void)
{
    test_pack_unpack_roundtrip();
    test_unpack_hostile();
    test_default();
    test_find_latest();
    test_next_write_slot();
    test_decide_write();
    test_record_crc();
    test_ct_cal_defaults_on_blank();
    test_ct_cal_corrupt_or_unknown_version();
    test_ct_cal_round_trip_and_independence();
    test_confirm_crc_ok();
    test_flash_rc_reason();
}
