// Host tests for firmware/SaftyFW/src/config_store.c -- the pure config
// record pack/unpack/CRC/versioning logic and the ARMED-write-refusal
// decision. No pico-sdk/FreeRTOS dependency, same discipline as
// test_bootloader_metadata.c (config_store.c mirrors bootloader/metadata.c's
// pattern almost exactly). config_store_flash.c (the real flash I/O and
// relay_owner ARMED check) is NOT covered here -- it needs a real RP2040,
// same reason update_task.c has no host test file.
#include <math.h>
#include <string.h>

#include "test_common.h"

#include "config_params.h"
#include "config_store.h"
#include "crc32.h" // bootloader/ -- to hand-assemble a legacy v1 record for the migration test
#include "kilnlink/kilnlink_config_page.h"

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

    // safety_tc_installed -- explicit 0 must roundtrip too, not just 1
    // (memset(0) above already made `rec` hold 0, so this is the meaningful
    // direction to check: an explicit "not installed" declaration must
    // survive a real pack/unpack, not just default to the safe value by
    // accident of being zeroed).
    TEST_CHECK(back.safety_tc_installed == 0u,
               "safety_tc_installed (explicit 0, declared not installed) roundtrips");
    rec.safety_tc_installed = 1u;
    config_store_pack(&rec, record);
    ok = config_store_unpack(record, &back);
    TEST_CHECK(ok && back.safety_tc_installed == 1u, "safety_tc_installed = 1 roundtrips");

    // Legacy-flash polarity -- THE CASE THAT ACTUALLY OCCURS ON REAL
    // HARDWARE, not the 0xFF case an earlier version of this test asserted
    // (confirmed live, 2026-08-23: that assertion was vacuous -- it proved
    // the decoder handles a byte value real flash never contains). What a
    // record written by firmware from BEFORE safety_tc_installed existed
    // actually holds at this offset is 0x00: old config_store_pack() did
    //     memcpy(&out[REC_OFF_RESERVED], rec->reserved, sizeof(rec->reserved))
    // with the OLD REC_OFF_RESERVED == 204 (this field's offset today) and
    // rec->reserved a 300-byte array that config_store_default() left at
    // memset(0) -- so out[204] = rec.reserved[0] = 0x00, not 0xFF. This is
    // simulated by hand below, calling this file's OWN pack() with the
    // record's reserved[0] forced to 0 and then hand-writing 0x00 at
    // offset 204 to stand in for "old firmware, which had no
    // safety_tc_installed field at all, wrote whatever its own reserved[0]
    // happened to be" -- CRC recomputed over it, matching how a real old
    // record's CRC legitimately covers that 0x00 byte.
    //
    // config_store_unpack() must decode 0x00 as installed (1) -- the whole
    // point of the positive-sentinel fix (SAFETY_TC_INSTALLED_MARKER_NOT_
    // INSTALLED, 0xA5): 0x00 is emphatically NOT that sentinel.
    rec.safety_tc_installed = 1u; // irrelevant to what gets written below -- overwritten by hand
    config_store_pack(&rec, record);
    record[204] = 0x00u; // REC_OFF_SAFETY_TC_INSTALLED -- the real legacy byte value
    {
        uint32_t crc = bootloader_crc32(record, 504u);
        record[504] = (uint8_t)(crc & 0xFFu);
        record[505] = (uint8_t)((crc >> 8) & 0xFFu);
        record[506] = (uint8_t)((crc >> 16) & 0xFFu);
        record[507] = (uint8_t)((crc >> 24) & 0xFFu);
    }
    ok = config_store_unpack(record, &back);
    TEST_CHECK(ok, "the simulated legacy record (0x00 at the safety_tc_installed offset, "
                    "CRC recomputed over it, matching real pre-existing flash) unpacks");
    TEST_CHECK(back.safety_tc_installed == 1u,
               "0x00 at the safety_tc_installed offset -- what every pre-existing board's "
               "flash actually contains -- decodes as installed (1), NOT as "
               "declared-not-installed. This is the check that matters: a board with a "
               "persisted config from before this field existed must come up as installed.");

    // Erased flash (0xFF) must ALSO decode as installed -- a record that
    // was never written at all (or genuinely does hold the old reserved-
    // fill byte from some other offset/path) must not accidentally trip
    // the not-installed sentinel either.
    config_store_pack(&rec, record);
    record[204] = 0xFFu;
    {
        uint32_t crc = bootloader_crc32(record, 504u);
        record[504] = (uint8_t)(crc & 0xFFu);
        record[505] = (uint8_t)((crc >> 8) & 0xFFu);
        record[506] = (uint8_t)((crc >> 16) & 0xFFu);
        record[507] = (uint8_t)((crc >> 24) & 0xFFu);
    }
    ok = config_store_unpack(record, &back);
    TEST_CHECK(ok && back.safety_tc_installed == 1u,
               "0xFF (erased flash) at the safety_tc_installed offset also decodes as installed");

    // Positive proof the sentinel itself still works: ONLY 0xA5 decodes as
    // not-installed. Uses config_store_pack()'s real encode path (rec.
    // safety_tc_installed = 0), not a hand-written byte, so this also
    // proves the encoder emits the sentinel this decode check depends on.
    rec.safety_tc_installed = 0u;
    config_store_pack(&rec, record);
    TEST_CHECK(record[204] == 0xA5u,
               "config_store_pack() encodes declared-not-installed as the explicit "
               "0xA5 sentinel, not as a bare 0x00");
    ok = config_store_unpack(record, &back);
    TEST_CHECK(ok && back.safety_tc_installed == 0u,
               "the 0xA5 sentinel round-trips back to declared-not-installed");

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
    TEST_CHECK(rec.safety_tc_installed == 1u,
               "default safety_tc_installed is 1 -- \"I expect a sensor and will trip if "
               "it's missing\", not the reverse");
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
    // REC_OFF_CRC = 504 as of format version 2), little-endian -- read
    // directly rather than duplicating the offset constant here, since this
    // test only needs to prove config_store_record_crc() agrees with what
    // pack() actually wrote.
    uint32_t expected = (uint32_t)packed[504] | ((uint32_t)packed[505] << 8) |
                         ((uint32_t)packed[506] << 16) | ((uint32_t)packed[507] << 24);

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

// --- v2: full-surface round trip, three-outcome load, unset-vs-zero -------
//
// These tests cover the format-version-2 growth pass (CONFIG_REFERENCE.md
// sections 1-5): a fully-populated record round-tripping through pack/
// unpack, a v1 record migrating forward with tc_type/ct_cal preserved and
// calibration_missing forced true, a hypothetical v3 record being refused
// rather than reinterpreted, and the fields_set bitmask keeping "nobody set
// this" distinguishable from "somebody set this to zero".

static void test_v2_full_roundtrip(void)
{
    TEST_SECTION("config_store v2 -- full-surface pack/unpack round trip");

    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    rec.seq = 77;
    rec.fields_set = (uint16_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_BORROWED_ZONE_INDEX |
                                 CONFIG_STORE_SET_TC_PLACEMENT_MODE | CONFIG_STORE_SET_ABS_MAX_TEMP_C |
                                 CONFIG_STORE_SET_CT_CHANNEL_MAP | CONFIG_STORE_SET_MAX_RATE_C_PER_MIN |
                                 CONFIG_STORE_SET_MAINS_VOLTAGE_V);

    rec.tc_source = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE;
    rec.borrowed_zone_index = 2;
    rec.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT;
    rec.abs_max_temp_c = 1310.5f;
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.ct_channel_map[0] = 0;
    rec.ct_channel_map[1] = 1;
    rec.ct_channel_map[2] = 2;
    rec.calibration_missing = false;

    rec.firing_margin_c = 111.0f;
    rec.overshoot_margin_c = 66.0f;
    rec.overshoot_time_s = 121u;
    rec.max_rate_c_per_min = 12.5f;
    rec.rate_window_s = 61u;
    rec.blind_grace_s = 62u;
    rec.frozen_window_s = 601u;
    rec.tc_disagreement_c = 199.0f;
    rec.tc_disagreement_time_s = 301u;
    rec.tc_expected_offset_c = 3.5f;
    rec.cj_warn_c = 59.0f;
    rec.cj_max_c = 84.0f;
    rec.cj_time_s = 61u;
    rec.borrowed_stale_s = 11u;
    rec.borrowed_stale_trip_s = 61u;
    rec.borrowed_type_expected = 0x05u; // MAX31856_TC_TYPE_R

    rec.i_present_a = 2.5f;
    rec.zero_counts[0] = 1000;
    rec.zero_counts[1] = 2000;
    rec.zero_counts[2] = 3000;
    rec.correlation_window_s = 151u;
    rec.stuck_on_time_s = 21u;
    rec.trip_verify_s = 11u;
    rec.k_ct_v_per_a[0] = 0.1f;
    rec.k_ct_v_per_a[1] = 0.2f;
    rec.k_ct_v_per_a[2] = 0.3f;
    rec.gain[0] = 0.700f;
    rec.gain[1] = 0.710f;
    rec.gain[2] = 0.720f;
    rec.mains_voltage_v = 240.0f;
    rec.power_window_s = 121u;

    rec.context_max_age_s = 6u;
    rec.link_timeout_s = 11u;
    rec.link_dead_hard_s = 121u;
    rec.mainfault_debounce_ms = 201u;
    rec.telemetry_period_ms = 501u;

    rec.startup_grace_s = 61u;
    rec.estop_debounce_ms = 51u;
    rec.watchdog_timeout_ms = 1001u;
    rec.config_check_period_s = 11u;

    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        rec.ct_cal[i].calibrated = true;
        rec.ct_cal[i].gain = 1.0f + (float)i * 0.1f;
        rec.ct_cal[i].offset = 0.01f * (float)i;
    }
    for (size_t i = 0; i < sizeof(rec.reserved); i++) {
        rec.reserved[i] = (uint8_t)(i + 3);
    }

    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);

    config_store_record_t back;
    memset(&back, 0xAA, sizeof(back));
    TEST_CHECK(config_store_unpack(record, &back), "fully-populated v2 record unpacks");

    TEST_CHECK(back.format_version == rec.format_version, "format_version roundtrips");
    TEST_CHECK(back.seq == rec.seq, "seq roundtrips");
    TEST_CHECK(back.fields_set == rec.fields_set, "fields_set roundtrips");
    TEST_CHECK(back.tc_source == rec.tc_source, "tc_source roundtrips");
    TEST_CHECK(back.borrowed_zone_index == rec.borrowed_zone_index, "borrowed_zone_index roundtrips");
    TEST_CHECK(back.tc_placement_mode == rec.tc_placement_mode, "tc_placement_mode roundtrips");
    TEST_CHECK(back.abs_max_temp_c == rec.abs_max_temp_c, "abs_max_temp_c roundtrips");
    TEST_CHECK(back.tc_type == rec.tc_type, "tc_type roundtrips");
    TEST_CHECK(memcmp(back.ct_channel_map, rec.ct_channel_map, sizeof(rec.ct_channel_map)) == 0,
               "ct_channel_map roundtrips");
    TEST_CHECK(back.calibration_missing == rec.calibration_missing, "calibration_missing roundtrips");

    TEST_CHECK(back.firing_margin_c == rec.firing_margin_c, "firing_margin_c roundtrips");
    TEST_CHECK(back.overshoot_margin_c == rec.overshoot_margin_c, "overshoot_margin_c roundtrips");
    TEST_CHECK(back.overshoot_time_s == rec.overshoot_time_s, "overshoot_time_s roundtrips");
    TEST_CHECK(back.max_rate_c_per_min == rec.max_rate_c_per_min, "max_rate_c_per_min roundtrips");
    TEST_CHECK(back.rate_window_s == rec.rate_window_s, "rate_window_s roundtrips");
    TEST_CHECK(back.blind_grace_s == rec.blind_grace_s, "blind_grace_s roundtrips");
    TEST_CHECK(back.frozen_window_s == rec.frozen_window_s, "frozen_window_s roundtrips");
    TEST_CHECK(back.tc_disagreement_c == rec.tc_disagreement_c, "tc_disagreement_c roundtrips");
    TEST_CHECK(back.tc_disagreement_time_s == rec.tc_disagreement_time_s,
               "tc_disagreement_time_s roundtrips");
    TEST_CHECK(back.tc_expected_offset_c == rec.tc_expected_offset_c, "tc_expected_offset_c roundtrips");
    TEST_CHECK(back.cj_warn_c == rec.cj_warn_c, "cj_warn_c roundtrips");
    TEST_CHECK(back.cj_max_c == rec.cj_max_c, "cj_max_c roundtrips");
    TEST_CHECK(back.cj_time_s == rec.cj_time_s, "cj_time_s roundtrips");
    TEST_CHECK(back.borrowed_stale_s == rec.borrowed_stale_s, "borrowed_stale_s roundtrips");
    TEST_CHECK(back.borrowed_stale_trip_s == rec.borrowed_stale_trip_s,
               "borrowed_stale_trip_s roundtrips");
    TEST_CHECK(back.borrowed_type_expected == rec.borrowed_type_expected,
               "borrowed_type_expected roundtrips");

    TEST_CHECK(back.i_present_a == rec.i_present_a, "i_present_a roundtrips");
    TEST_CHECK(memcmp(back.zero_counts, rec.zero_counts, sizeof(rec.zero_counts)) == 0,
               "zero_counts roundtrips");
    TEST_CHECK(back.correlation_window_s == rec.correlation_window_s,
               "correlation_window_s roundtrips");
    TEST_CHECK(back.stuck_on_time_s == rec.stuck_on_time_s, "stuck_on_time_s roundtrips");
    TEST_CHECK(back.trip_verify_s == rec.trip_verify_s, "trip_verify_s roundtrips");
    for (size_t i = 0; i < 3; i++) {
        TEST_CHECK(back.k_ct_v_per_a[i] == rec.k_ct_v_per_a[i], "k_ct_v_per_a[i] roundtrips");
        TEST_CHECK(back.gain[i] == rec.gain[i], "gain[i] roundtrips");
    }
    TEST_CHECK(back.mains_voltage_v == rec.mains_voltage_v, "mains_voltage_v roundtrips");
    TEST_CHECK(back.power_window_s == rec.power_window_s, "power_window_s roundtrips");

    TEST_CHECK(back.context_max_age_s == rec.context_max_age_s, "context_max_age_s roundtrips");
    TEST_CHECK(back.link_timeout_s == rec.link_timeout_s, "link_timeout_s roundtrips");
    TEST_CHECK(back.link_dead_hard_s == rec.link_dead_hard_s, "link_dead_hard_s roundtrips");
    TEST_CHECK(back.mainfault_debounce_ms == rec.mainfault_debounce_ms,
               "mainfault_debounce_ms roundtrips");
    TEST_CHECK(back.telemetry_period_ms == rec.telemetry_period_ms, "telemetry_period_ms roundtrips");

    TEST_CHECK(back.startup_grace_s == rec.startup_grace_s, "startup_grace_s roundtrips");
    TEST_CHECK(back.estop_debounce_ms == rec.estop_debounce_ms, "estop_debounce_ms roundtrips");
    TEST_CHECK(back.watchdog_timeout_ms == rec.watchdog_timeout_ms, "watchdog_timeout_ms roundtrips");
    TEST_CHECK(back.config_check_period_s == rec.config_check_period_s,
               "config_check_period_s roundtrips");

    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        TEST_CHECK(back.ct_cal[i].calibrated == rec.ct_cal[i].calibrated,
                   "ct_cal[i].calibrated roundtrips (v2 full record)");
        TEST_CHECK(back.ct_cal[i].gain == rec.ct_cal[i].gain, "ct_cal[i].gain roundtrips (v2 full record)");
        TEST_CHECK(back.ct_cal[i].offset == rec.ct_cal[i].offset,
                   "ct_cal[i].offset roundtrips (v2 full record)");
    }
    TEST_CHECK(memcmp(back.reserved, rec.reserved, sizeof(rec.reserved)) == 0,
               "reserved bytes roundtrip byte-for-byte (v2 full record)");
}

// A hand-assembled legacy v1 record: format_version 1, magic, seq, tc_type,
// calibration_missing, ct_cal[3], CRC over bytes [0, 248) -- exactly the
// byte layout the original (pre-this-pass) config_store_pack() produced.
// Kept local to this test file (not shared with config_store.c) because its
// entire purpose is to construct bytes that today's config_store_pack() can
// no longer produce, so the migration path has something real to migrate.
static void pack_legacy_v1_record(uint32_t seq, uint8_t tc_type, bool calibration_missing,
                                   const config_store_ct_channel_cal_t ct_cal[3],
                                   uint8_t out[CONFIG_STORE_RECORD_LEN])
{
    memset(out, 0xFF, CONFIG_STORE_RECORD_LEN);
    out[0] = (uint8_t)(CONFIG_STORE_MAGIC & 0xFFu);
    out[1] = (uint8_t)((CONFIG_STORE_MAGIC >> 8) & 0xFFu);
    out[2] = (uint8_t)((CONFIG_STORE_MAGIC >> 16) & 0xFFu);
    out[3] = (uint8_t)((CONFIG_STORE_MAGIC >> 24) & 0xFFu);
    out[4] = (uint8_t)(CONFIG_STORE_FORMAT_VERSION_V1 & 0xFFu);
    out[5] = (uint8_t)((CONFIG_STORE_FORMAT_VERSION_V1 >> 8) & 0xFFu);
    out[6] = 0;
    out[7] = 0;
    out[8] = (uint8_t)(seq & 0xFFu);
    out[9] = (uint8_t)((seq >> 8) & 0xFFu);
    out[10] = (uint8_t)((seq >> 16) & 0xFFu);
    out[11] = (uint8_t)((seq >> 24) & 0xFFu);
    out[12] = tc_type;
    out[13] = calibration_missing ? 1u : 0u;
    out[14] = 0;
    out[15] = 0;
    for (unsigned ch = 0; ch < 3u; ch++) {
        size_t off = 16u + (size_t)ch * 9u;
        union {
            float    f;
            uint32_t u;
        } g, o;
        g.f = ct_cal[ch].gain;
        o.f = ct_cal[ch].offset;
        out[off] = ct_cal[ch].calibrated ? 1u : 0u;
        out[off + 1] = (uint8_t)(g.u & 0xFFu);
        out[off + 2] = (uint8_t)((g.u >> 8) & 0xFFu);
        out[off + 3] = (uint8_t)((g.u >> 16) & 0xFFu);
        out[off + 4] = (uint8_t)((g.u >> 24) & 0xFFu);
        out[off + 5] = (uint8_t)(o.u & 0xFFu);
        out[off + 6] = (uint8_t)((o.u >> 8) & 0xFFu);
        out[off + 7] = (uint8_t)((o.u >> 16) & 0xFFu);
        out[off + 8] = (uint8_t)((o.u >> 24) & 0xFFu);
    }
    uint32_t crc = bootloader_crc32(out, 248u);
    out[248] = (uint8_t)(crc & 0xFFu);
    out[249] = (uint8_t)((crc >> 8) & 0xFFu);
    out[250] = (uint8_t)((crc >> 16) & 0xFFu);
    out[251] = (uint8_t)((crc >> 24) & 0xFFu);
}

static void test_v1_migration(void)
{
    TEST_SECTION("config_store -- v1 -> v2 migration");

    config_store_ct_channel_cal_t ct_cal[3];
    ct_cal[0].calibrated = true;
    ct_cal[0].gain = 1.05f;
    ct_cal[0].offset = -0.02f;
    ct_cal[1].calibrated = false;
    ct_cal[1].gain = 0.0f;
    ct_cal[1].offset = 0.0f;
    ct_cal[2].calibrated = true;
    ct_cal[2].gain = 0.97f;
    ct_cal[2].offset = 0.03f;

    uint8_t v1_record[CONFIG_STORE_RECORD_LEN];
    // A v1 record that was NEVER commissioned (calibration_missing false in
    // this test's own input) still must migrate to calibration_missing ==
    // true -- proving the flag is FORCED true by the migration path itself,
    // not merely carried through from whatever v1 happened to hold.
    pack_legacy_v1_record(15u, 0x07u /* MAX31856_TC_TYPE_T */, false, ct_cal, v1_record);

    config_store_record_t out;
    memset(&out, 0xAA, sizeof(out));
    bool ok = config_store_unpack(v1_record, &out);
    TEST_CHECK(ok, "a well-formed legacy v1 record migrates successfully");
    TEST_CHECK(out.format_version == CONFIG_STORE_FORMAT_VERSION,
               "migrated record reads back as the current format_version");
    TEST_CHECK(out.seq == 15u, "migrated record's seq is preserved from v1");
    TEST_CHECK(out.tc_type == 0x07u, "migrated record's tc_type is preserved from v1");
    TEST_CHECK(out.calibration_missing == true,
               "migrated record has calibration_missing forced true, even though the v1 "
               "record it came from held false -- a migrated record was never commissioned "
               "against the fields this pass added");
    TEST_CHECK(out.ct_cal[0].calibrated == true && out.ct_cal[0].gain == 1.05f &&
                   out.ct_cal[0].offset == -0.02f,
               "migrated record's ct_cal[0] is preserved from v1");
    TEST_CHECK(out.ct_cal[1].calibrated == false, "migrated record's ct_cal[1] (uncalibrated) preserved");
    TEST_CHECK(out.ct_cal[2].calibrated == true && out.ct_cal[2].gain == 0.97f &&
                   out.ct_cal[2].offset == 0.03f,
               "migrated record's ct_cal[2] is preserved from v1");

    // Everything v1 never had must come back at config_store_default()'s
    // compiled default, not at zero-that-happens-to-look-like-a-default.
    config_store_record_t def;
    config_store_default(&def);
    TEST_CHECK(out.firing_margin_c == def.firing_margin_c,
               "migrated record's firing_margin_c takes the v2 compiled default");
    TEST_CHECK(out.watchdog_timeout_ms == def.watchdog_timeout_ms,
               "migrated record's watchdog_timeout_ms takes the v2 compiled default");
    TEST_CHECK(out.fields_set == 0,
               "migrated record has fields_set == 0 -- none of the no-safe-default fields "
               "were ever commissioned, v1 could not have set them");

    // Prove this check can actually fail: corrupt the v1 record's CRC and
    // confirm migration is refused, not silently accepted with garbage.
    uint8_t corrupted[CONFIG_STORE_RECORD_LEN];
    memcpy(corrupted, v1_record, sizeof(corrupted));
    corrupted[20] ^= 0x01u; // inside ct_cal, well before the v1 CRC at byte 248
    config_store_record_t sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));
    config_store_record_t out2 = sentinel;
    TEST_CHECK(!config_store_unpack(corrupted, &out2),
               "a corrupted legacy v1 record is refused, not migrated with garbage");
    TEST_CHECK(memcmp(&out2, &sentinel, sizeof(out2)) == 0,
               "*out untouched after a corrupted v1 record is refused");
}

static void test_future_version_refused(void)
{
    TEST_SECTION("config_store -- a future (v3) record is refused, never reinterpreted");

    // Build a record that is perfectly well-formed EXCEPT its format_version
    // claims a version newer than anything this firmware understands. Its
    // CRC is computed over the actual bytes (including that version field),
    // so this is NOT a CRC-mismatch case -- it must be refused specifically
    // because of the version, proving the two checks are independent.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.format_version = (uint16_t)(CONFIG_STORE_FORMAT_VERSION + 1u); // "v3"
    rec.seq = 999u;
    rec.abs_max_temp_c = 1234.0f; // a plausible-looking value a v2 reader
                                  // must NOT be tempted to trust if it
                                  // ignored the version check

    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);

    config_store_record_t sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));
    config_store_record_t out = sentinel;
    TEST_CHECK(!config_store_unpack(record, &out),
               "a record with a newer-than-known format_version is refused");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0,
               "*out is left completely untouched when a future version is refused");

    // Also prove find_latest() treats a too-new slot the same as corrupt --
    // skipped, never chosen, even when it has the highest seq in the sector.
    uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(&sector[0], record, CONFIG_STORE_RECORD_LEN); // slot 0: seq 999, v3 -- must be skipped

    config_store_record_t older;
    config_store_default(&older);
    older.format_version = CONFIG_STORE_FORMAT_VERSION;
    older.seq = 5u;
    uint8_t older_record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&older, older_record);
    memcpy(&sector[1 * CONFIG_STORE_RECORD_LEN], older_record, CONFIG_STORE_RECORD_LEN);

    config_store_record_t found;
    size_t slot = config_store_find_latest(sector, &found);
    TEST_CHECK(slot == 1, "the too-new slot (higher seq) is skipped; the valid v2 slot wins");
    TEST_CHECK(found.seq == 5u, "find_latest() returns the older, valid record's contents");
}

static void test_unset_fields_distinguishable_from_zero(void)
{
    TEST_SECTION("config_store -- fields_set keeps 'unset' distinguishable from 'set to zero'");

    // A field that is explicitly set to a value equal to what an unset
    // field would numerically read as (0 / 0.0f) must still be
    // distinguishable via fields_set -- proving this is a real flag, not a
    // sentinel value that happens to collide with a valid reading.
    config_store_record_t rec;
    config_store_default(&rec);
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) == 0,
               "default record: abs_max_temp_c starts unset");
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) == 0,
               "default record: max_rate_c_per_min starts unset");
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_MAINS_VOLTAGE_V) == 0,
               "default record: mains_voltage_v starts unset");

    // Deliberately commission abs_max_temp_c to exactly 0.0 -- the one value
    // that would be indistinguishable from "unset" under a sentinel scheme
    // using 0 as the sentinel.
    rec.abs_max_temp_c = 0.0f;
    rec.fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C;
    // max_rate_c_per_min and mains_voltage_v deliberately left at 0.0f AND
    // unset, to prove the two "reads as 0.0" cases differ only in the bit.
    TEST_CHECK(rec.max_rate_c_per_min == 0.0f && (rec.fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) == 0,
               "max_rate_c_per_min: value 0.0 but genuinely unset");

    uint8_t record[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, record);
    config_store_record_t back;
    TEST_CHECK(config_store_unpack(record, &back), "record with a zero-but-set field unpacks");

    TEST_CHECK((back.fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) != 0,
               "abs_max_temp_c==0.0-but-SET roundtrips as set");
    TEST_CHECK(back.abs_max_temp_c == 0.0f, "abs_max_temp_c's value (0.0) roundtrips too");
    TEST_CHECK((back.fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) == 0,
               "max_rate_c_per_min==0.0-and-UNSET still roundtrips as unset -- proves the bit, "
               "not the numeric value, is what a caller must trust");

    // Prove config_store_field_is_set() itself can fail: flip a bit off and
    // confirm the helper reports not-set; flip it back on and confirm set.
    uint16_t fields_set = back.fields_set;
    TEST_CHECK(config_store_field_is_set(&fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C),
               "config_store_field_is_set(): true when the bit is present");
    fields_set = (uint16_t)(fields_set & ~(uint16_t)CONFIG_STORE_SET_ABS_MAX_TEMP_C);
    TEST_CHECK(!config_store_field_is_set(&fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C),
               "config_store_field_is_set(): false once the bit is cleared -- proves the "
               "check can actually fail, not just always return true");
}

// --- config_params.c: SET_PARAM/GET_PARAM/COMMIT_CONFIG/GET_CONFIG_PAGE's
// pure id<->field mapping, cross-field validation, and the ct_channel_map
// group-bit derivation (docs/COMMISSIONING.md section 2/2.1).

static void test_config_params_get_set_roundtrip(void)
{
    TEST_SECTION("config_params_get/set -- roundtrip across every wire type");

    config_store_record_t rec;
    config_store_default(&rec);

    // U8, gated (tc_source).
    kilnlink_param_value_t v;
    v.u8_val = CONFIG_STORE_TC_SOURCE_BOTH;
    TEST_CHECK(config_params_set(&rec, 0x0101u, KILNLINK_PARAM_TYPE_U8, v), "set tc_source (U8)");
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_TC_SOURCE) != 0, "tc_source gates fields_set");
    uint8_t type = 0;
    kilnlink_param_value_t got;
    TEST_CHECK(config_params_get(&rec, 0x0101u, &type, &got), "get tc_source");
    TEST_CHECK(type == KILNLINK_PARAM_TYPE_U8 && got.u8_val == CONFIG_STORE_TC_SOURCE_BOTH,
               "tc_source value/type roundtrips");

    // F32, gated (abs_max_temp_c).
    v.f32_val = 1305.25f;
    TEST_CHECK(config_params_set(&rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v), "set abs_max_temp_c (F32)");
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) != 0, "abs_max_temp_c gates fields_set");
    TEST_CHECK(config_params_get(&rec, 0x0104u, &type, &got), "get abs_max_temp_c");
    TEST_CHECK(type == KILNLINK_PARAM_TYPE_F32 && got.f32_val == 1305.25f, "abs_max_temp_c roundtrips");

    // U16 wire <-> u32 record (blind_grace_s), NOT gated.
    v.u16_val = 42u;
    TEST_CHECK(config_params_set(&rec, 0x0206u, KILNLINK_PARAM_TYPE_U16, v), "set blind_grace_s (U16)");
    TEST_CHECK(rec.blind_grace_s == 42u, "blind_grace_s stored as the record's own u32");
    TEST_CHECK(config_params_get(&rec, 0x0206u, &type, &got), "get blind_grace_s");
    TEST_CHECK(type == KILNLINK_PARAM_TYPE_U16 && got.u16_val == 42u, "blind_grace_s roundtrips as U16");

    // BOOL (ct_cal[1].calibrated).
    v.bool_val = 1u;
    TEST_CHECK(config_params_set(&rec, 0x0317u, KILNLINK_PARAM_TYPE_BOOL, v), "set ct_cal[1].calibrated (BOOL)");
    TEST_CHECK(rec.ct_cal[1].calibrated == true, "ct_cal[1].calibrated stored");
    TEST_CHECK(config_params_get(&rec, 0x0317u, &type, &got), "get ct_cal[1].calibrated");
    TEST_CHECK(type == KILNLINK_PARAM_TYPE_BOOL && got.bool_val == 1u, "ct_cal[1].calibrated roundtrips");
}

static void test_config_params_unknown_id_refused(void)
{
    TEST_SECTION("config_params_get/set -- unknown param_id refused, not guessed at");

    config_store_record_t rec;
    config_store_default(&rec);
    config_store_record_t sentinel = rec;

    kilnlink_param_value_t v;
    v.u8_val = 5u;
    TEST_CHECK(!config_params_set(&rec, 0xFFFFu, KILNLINK_PARAM_TYPE_U8, v),
               "an id nothing in this table names is refused by set()");
    TEST_CHECK(memcmp(&rec, &sentinel, sizeof(rec)) == 0, "set() leaves rec untouched on an unknown id");

    uint8_t type = 0xAAu;
    kilnlink_param_value_t got;
    memset(&got, 0xAA, sizeof(got));
    TEST_CHECK(!config_params_get(&rec, 0xFFFFu, &type, &got),
               "an id nothing in this table names is refused by get() -- "
               "the wire's GET_PARAM reply turns this into found=0");
}

static void test_config_params_type_mismatch_refused(void)
{
    TEST_SECTION("config_params_set -- wrong wire type for a real id is refused, not coerced");

    config_store_record_t rec;
    config_store_default(&rec);
    config_store_record_t sentinel = rec;

    // blind_grace_s (0x0206) is U16 -- sending an F32 for it must be refused
    // wholesale, never reinterpreted as some other numeric value.
    kilnlink_param_value_t v;
    v.f32_val = 3.14f;
    TEST_CHECK(!config_params_set(&rec, 0x0206u, KILNLINK_PARAM_TYPE_F32, v),
               "an F32 value for a U16 field is refused");
    TEST_CHECK(memcmp(&rec, &sentinel, sizeof(rec)) == 0, "rec is untouched on a type mismatch");
}

static void test_config_params_set_range_validation(void)
{
    TEST_SECTION("config_params_set -- range/finiteness validation at SET_PARAM time");

    config_store_record_t rec;
    kilnlink_param_value_t v;

    // --- Enums: an out-of-range value is refused, rec left untouched -------
    config_store_default(&rec);
    config_store_record_t sentinel = rec;
    v.u8_val = 3u; // tc_source only has 0/1/2 (OWN_J7/BORROWED_ZONE/BOTH)
    TEST_CHECK(!config_params_set(&rec, 0x0101u, KILNLINK_PARAM_TYPE_U8, v),
               "tc_source = 3 (one past BOTH) is refused");
    TEST_CHECK(memcmp(&rec, &sentinel, sizeof(rec)) == 0, "rec untouched by the refused tc_source");

    // Prove it can PASS too: every named tc_source member is accepted.
    for (uint8_t ok = CONFIG_STORE_TC_SOURCE_OWN_J7; ok <= CONFIG_STORE_TC_SOURCE_BOTH; ok++) {
        v.u8_val = ok;
        TEST_CHECK(config_params_set(&rec, 0x0101u, KILNLINK_PARAM_TYPE_U8, v),
                   "every named tc_source value is accepted");
    }

    config_store_default(&rec);
    v.u8_val = 3u; // CONFIG_REFERENCE.md sec1: borrowed_zone_index is "0-2"
    TEST_CHECK(!config_params_set(&rec, 0x0102u, KILNLINK_PARAM_TYPE_U8, v),
               "borrowed_zone_index = 3 is refused (documented range is 0-2)");
    for (uint8_t ok = 0u; ok <= 2u; ok++) {
        v.u8_val = ok;
        TEST_CHECK(config_params_set(&rec, 0x0102u, KILNLINK_PARAM_TYPE_U8, v),
                   "borrowed_zone_index 0, 1, and 2 are all accepted");
    }

    config_store_default(&rec);
    v.u8_val = 2u; // only CHAMBER_AGREED(0)/EXTERNAL_OVERHEAT(1) exist
    TEST_CHECK(!config_params_set(&rec, 0x0103u, KILNLINK_PARAM_TYPE_U8, v),
               "tc_placement_mode = 2 (past EXTERNAL_OVERHEAT) is refused");
    v.u8_val = CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT;
    TEST_CHECK(config_params_set(&rec, 0x0103u, KILNLINK_PARAM_TYPE_U8, v),
               "tc_placement_mode = EXTERNAL_OVERHEAT (the boundary value) is accepted");

    config_store_default(&rec);
    v.u8_val = 8u; // MAX31856_TC_TYPE_* only runs 0-7 (B..T)
    TEST_CHECK(!config_params_set(&rec, 0x0105u, KILNLINK_PARAM_TYPE_U8, v),
               "tc_type = 8 (past T) is refused");
    v.u8_val = 7u;
    TEST_CHECK(config_params_set(&rec, 0x0105u, KILNLINK_PARAM_TYPE_U8, v),
               "tc_type = 7 (T, the boundary value) is accepted");

    config_store_default(&rec);
    v.u8_val = 8u;
    TEST_CHECK(!config_params_set(&rec, 0x0210u, KILNLINK_PARAM_TYPE_U8, v),
               "borrowed_type_expected = 8 (past T) is refused");
    v.u8_val = 7u;
    TEST_CHECK(config_params_set(&rec, 0x0210u, KILNLINK_PARAM_TYPE_U8, v),
               "borrowed_type_expected = 7 (T, the boundary value) is accepted");

    // safety_tc_installed (0x0211) -- 0/1 only, no third state.
    config_store_default(&rec);
    v.u8_val = 2u;
    TEST_CHECK(!config_params_set(&rec, 0x0211u, KILNLINK_PARAM_TYPE_U8, v),
               "safety_tc_installed = 2 is refused (0/1 only)");
    v.u8_val = 1u;
    TEST_CHECK(config_params_set(&rec, 0x0211u, KILNLINK_PARAM_TYPE_U8, v),
               "safety_tc_installed = 1 (installed, the boundary/default value) is accepted");
    v.u8_val = 0u;
    TEST_CHECK(config_params_set(&rec, 0x0211u, KILNLINK_PARAM_TYPE_U8, v),
               "safety_tc_installed = 0 (declared not installed) is accepted");
    TEST_CHECK(rec.safety_tc_installed == 0u, "the declared-not-installed value actually lands in the record");

    // --- Floats: NaN/+Inf/-Inf refused for EVERY F32 field ------------------
    // abs_max_temp_c is the single most dangerous field in this table -- a
    // NaN ceiling is a guard (S1) that can never trip, since `x > NaN` is
    // always false. Prove all three non-finite bit patterns are refused,
    // then prove a normal in-range value still works (not an over-tight
    // validator).
    static const uint16_t f32_ids[] = {
        0x0104u, // abs_max_temp_c
        0x0201u, // firing_margin_c
        0x0202u, // overshoot_margin_c
        0x0204u, // max_rate_c_per_min
        0x0208u, // tc_disagreement_c
        0x020Au, // tc_expected_offset_c
        0x020Bu, // cj_warn_c
        0x020Cu, // cj_max_c
        0x0301u, // i_present_a
        0x0308u, // k_ct_v_per_a[0]
        0x0309u, // k_ct_v_per_a[1]
        0x030Au, // k_ct_v_per_a[2]
        0x030Bu, // gain[0]
        0x030Cu, // gain[1]
        0x030Du, // gain[2]
        0x030Eu, // mains_voltage_v
        0x0310u, // ct_cal[0].gain
        0x0311u, // ct_cal[1].gain
        0x0312u, // ct_cal[2].gain
        0x0313u, // ct_cal[0].offset
        0x0314u, // ct_cal[1].offset
        0x0315u, // ct_cal[2].offset
    };
    for (size_t i = 0; i < sizeof(f32_ids) / sizeof(f32_ids[0]); i++) {
        config_store_default(&rec);
        config_store_record_t before = rec;
        v.f32_val = NAN;
        TEST_CHECK(!config_params_set(&rec, f32_ids[i], KILNLINK_PARAM_TYPE_F32, v),
                   "NaN is refused for this F32 field");
        TEST_CHECK(memcmp(&rec, &before, sizeof(rec)) == 0, "rec untouched by a refused NaN");

        v.f32_val = INFINITY;
        TEST_CHECK(!config_params_set(&rec, f32_ids[i], KILNLINK_PARAM_TYPE_F32, v),
                   "+Inf is refused for this F32 field");

        v.f32_val = -INFINITY;
        TEST_CHECK(!config_params_set(&rec, f32_ids[i], KILNLINK_PARAM_TYPE_F32, v),
                   "-Inf is refused for this F32 field");

        // Sanity: a normal finite value is still accepted -- proves this
        // isn't an over-tight validator rejecting legitimate values too.
        v.f32_val = 12.5f;
        TEST_CHECK(config_params_set(&rec, f32_ids[i], KILNLINK_PARAM_TYPE_F32, v),
                   "an ordinary finite value is still accepted for this F32 field");
    }

    // --- Negative ceilings/thresholds: the two fields CONFIG_REFERENCE.md ---
    // explicitly bounds below by zero (abs_max_temp_c section 1,
    // i_present_a section 3). safety_guards.c once accepted a finite
    // NEGATIVE ceiling via `isfinite(x) && x < abs_max` with no lower bound
    // -- this is the same class of bug, closed here instead.
    config_store_default(&rec);
    v.f32_val = -1.0f;
    TEST_CHECK(!config_params_set(&rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v),
               "a negative abs_max_temp_c ceiling is refused");
    v.f32_val = 0.0f;
    TEST_CHECK(config_params_set(&rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v),
               "abs_max_temp_c = 0.0 (the boundary value) is accepted");
    v.f32_val = 1305.0f;
    TEST_CHECK(config_params_set(&rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v),
               "a realistic positive abs_max_temp_c (~cone 10) is accepted");

    config_store_default(&rec);
    v.f32_val = -0.5f;
    TEST_CHECK(!config_params_set(&rec, 0x0301u, KILNLINK_PARAM_TYPE_F32, v),
               "a negative i_present_a threshold is refused");
    v.f32_val = 2.0f;
    TEST_CHECK(config_params_set(&rec, 0x0301u, KILNLINK_PARAM_TYPE_F32, v),
               "the documented default i_present_a (2.0 A) is accepted");

    // --- Deliberately-unbounded floats: NOT rejected for being negative or -
    // unusual, since CONFIG_REFERENCE.md documents no sign/magnitude bound
    // for them (tc_expected_offset_c is explicitly a signed offset).
    config_store_default(&rec);
    v.f32_val = -15.0f;
    TEST_CHECK(config_params_set(&rec, 0x020Au, KILNLINK_PARAM_TYPE_F32, v),
               "tc_expected_offset_c is signed by design -- a negative value is NOT rejected");
}

static void test_config_params_validate_range_validation(void)
{
    TEST_SECTION("config_params_validate -- range re-check as a backstop against non-SET_PARAM records");

    // Simulates a record that reached COMMIT_CONFIG without going through
    // config_params_set() for every field (e.g. hand-built, or migrated
    // forward from some future format) -- config_params_validate() must
    // still catch an impossible value even though config_params_set() never
    // saw it.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.abs_max_temp_c = NAN;

    const char *field = NULL;
    const char *rule = NULL;
    TEST_CHECK(!config_params_validate(&rec, &field, &rule),
               "a NaN abs_max_temp_c fails validate() even without going through set()");
    TEST_CHECK(field != NULL && strcmp(field, "abs_max_temp_c") == 0, "the offending field is named");

    config_store_default(&rec);
    rec.tc_source = 9u; // never a value SET_PARAM's own check would let through
    field = NULL;
    TEST_CHECK(!config_params_validate(&rec, &field, &rule),
               "an out-of-range tc_source fails validate() even without going through set()");
    TEST_CHECK(field != NULL && strcmp(field, "tc_source") == 0, "the offending field is named");

    // Sanity: an ordinary, fully-defaulted record still validates -- this
    // backstop must not turn into its own over-tight validator.
    config_store_default(&rec);
    field = NULL;
    rule = NULL;
    TEST_CHECK(config_params_validate(&rec, &field, &rule),
               "an untouched, compiled-default record still validates");
}

static void test_config_params_validate_ex_reason_and_id_lookup(void)
{
    TEST_SECTION("config_params_validate_ex / config_params_id_for_field_name -- "
                 "COMMIT_CONFIG_REJECTED's (0x20) wire-sized reason and param_id");

    // A range failure reports CONFIG_PARAMS_REJECT_RANGE, and the offending
    // field's name maps to its real COMMISSIONING.md sec 2.1 param_id.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.abs_max_temp_c = NAN;

    const char *field = NULL;
    const char *rule = NULL;
    config_params_reject_reason_t reason = CONFIG_PARAMS_REJECT_CONTRADICTION; // poison, must be overwritten
    TEST_CHECK(!config_params_validate_ex(&rec, &field, &rule, &reason),
               "NaN abs_max_temp_c still fails validate_ex()");
    TEST_CHECK(reason == CONFIG_PARAMS_REJECT_RANGE, "a range failure reports CONFIG_PARAMS_REJECT_RANGE");
    TEST_CHECK(config_params_id_for_field_name(field) == 0x0104u,
               "abs_max_temp_c's name maps to its real param_id 0x0104");

    // A contradiction reports CONFIG_PARAMS_REJECT_CONTRADICTION, not RANGE
    // -- this is the check that actually distinguishes the two wire reasons;
    // without it, every rejection would look identical to the ESP.
    config_store_default(&rec);
    rec.tc_source = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE;
    rec.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT;
    rec.fields_set |= (uint16_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_TC_PLACEMENT_MODE);
    field = NULL;
    rule = NULL;
    reason = CONFIG_PARAMS_REJECT_RANGE; // poison, must be overwritten
    TEST_CHECK(!config_params_validate_ex(&rec, &field, &rule, &reason),
               "the tc_placement_mode/tc_source contradiction still fails validate_ex()");
    TEST_CHECK(reason == CONFIG_PARAMS_REJECT_CONTRADICTION,
               "a cross-field contradiction reports CONFIG_PARAMS_REJECT_CONTRADICTION, not RANGE");
    TEST_CHECK(config_params_id_for_field_name(field) == 0x0103u,
               "tc_placement_mode's name maps to its real param_id 0x0103");

    // A passing record leaves *out_reason untouched at NONE and old
    // callers (out_reason == NULL, i.e. config_params_validate() itself)
    // still work unchanged.
    config_store_default(&rec);
    field = NULL;
    rule = NULL;
    reason = CONFIG_PARAMS_REJECT_RANGE; // poison
    TEST_CHECK(config_params_validate_ex(&rec, &field, &rule, &reason), "a clean record passes validate_ex()");
    TEST_CHECK(reason == CONFIG_PARAMS_REJECT_NONE, "a passing validate_ex() reports CONFIG_PARAMS_REJECT_NONE");
    TEST_CHECK(config_params_validate(&rec, &field, &rule), "config_params_validate() wrapper still works");

    // An unknown/NULL name is the NO_PARAM_ID sentinel, never a 0 or a
    // garbage id that could collide with a real one.
    TEST_CHECK(config_params_id_for_field_name("not_a_real_field") == CONFIG_PARAMS_NO_PARAM_ID,
               "an unrecognised field name maps to the NO_PARAM_ID sentinel");
    TEST_CHECK(config_params_id_for_field_name(NULL) == CONFIG_PARAMS_NO_PARAM_ID,
               "a NULL field name maps to the NO_PARAM_ID sentinel");
    TEST_CHECK(config_params_id_for_field_name("rec") == CONFIG_PARAMS_NO_PARAM_ID,
               "\"rec\" (the NULL-record guard's own out_field) is not a real param and maps to the sentinel");
}

static void test_config_params_id_table_self_consistent(void)
{
    TEST_SECTION("config_params -- every id in the enumeration table is get()/set()-able");

    // Proves the id table config_params_count()/_id_at() expose for
    // GET_CONFIG_PAGE agrees with the switch statements config_params_get()/
    // _set() actually dispatch on -- a real risk in a hand-written table
    // plus a hand-written switch, and exactly the kind of drift that would
    // otherwise silently drop an id from every page dump while still
    // accepting SET_PARAM for it (or vice versa).
    config_store_record_t rec;
    config_store_default(&rec);

    size_t count = config_params_count();
    TEST_CHECK(count > 0, "the id table is non-empty");
    for (size_t i = 0; i < count; i++) {
        uint16_t id = 0;
        uint8_t table_type = 0;
        TEST_CHECK(config_params_id_at(i, &id, &table_type), "id_at() succeeds for every in-range index");

        uint8_t got_type = 0;
        kilnlink_param_value_t got;
        memset(&got, 0, sizeof(got));
        TEST_CHECK(config_params_get(&rec, id, &got_type, &got),
                   "every id the table lists is recognised by get()");
        TEST_CHECK(got_type == table_type, "get()'s reported type matches the table's own type for this id");
    }
    TEST_CHECK(!config_params_id_at(count, NULL, NULL), "one past the end is out of range");
}

static void test_config_params_validate_contradiction_rejected(void)
{
    TEST_SECTION("config_params_validate -- tc_placement_mode vs tc_source contradiction rejected");

    // Both staged, contradictory: BORROWED_ZONE forces CHAMBER_AGREED
    // (CONFIG_REFERENCE.md sec1); EXTERNAL_OVERHEAT must be rejected, not
    // silently reconciled to CHAMBER_AGREED.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_source = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE;
    rec.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT;
    rec.fields_set |= (uint16_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_TC_PLACEMENT_MODE);

    const char *field = NULL;
    const char *rule = NULL;
    TEST_CHECK(!config_params_validate(&rec, &field, &rule),
               "BORROWED_ZONE + EXTERNAL_OVERHEAT, both staged, is rejected");
    TEST_CHECK(field != NULL && strcmp(field, "tc_placement_mode") == 0,
               "the offending field is named");
    TEST_CHECK(rule != NULL && strlen(rule) > 0, "the violated rule is named");

    // Prove this check can actually PASS too: same tc_source, but
    // CHAMBER_AGREED -- the combination the rule forces -- must validate.
    rec.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    field = NULL;
    rule = NULL;
    TEST_CHECK(config_params_validate(&rec, &field, &rule),
               "BORROWED_ZONE + CHAMBER_AGREED (the forced combination) validates");

    // And prove the check does NOT fire when only ONE of the two fields has
    // actually been staged -- an unset field cannot contradict anything, and
    // a commit that only touches unrelated fields must not be blocked by
    // whatever zero-initialised tc_placement_mode happens to read as.
    config_store_record_t partial;
    config_store_default(&partial);
    partial.tc_source = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE;
    partial.fields_set |= CONFIG_STORE_SET_TC_SOURCE; // tc_placement_mode left UNSET
    TEST_CHECK(config_params_validate(&partial, &field, &rule),
               "tc_source alone (tc_placement_mode still unset) does not trip the contradiction check");
}

static void test_config_params_ct_channel_map_two_of_three(void)
{
    TEST_SECTION("config_params -- ct_channel_map: two of three channels leaves the group bit unset");

    config_store_record_t rec;
    config_store_default(&rec);

    kilnlink_param_value_t v;
    v.u8_val = 0u;
    TEST_CHECK(config_params_set(&rec, 0x0106u, KILNLINK_PARAM_TYPE_U8, v), "stage channel 0");
    v.u8_val = 1u;
    TEST_CHECK(config_params_set(&rec, 0x0107u, KILNLINK_PARAM_TYPE_U8, v), "stage channel 1");
    // Channel 2 (0x0108) deliberately NOT staged.

    config_params_finalize_ct_channel_map(&rec);
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_CT_CHANNEL_MAP) == 0,
               "two of three channels staged: the group bit stays UNSET");
    TEST_CHECK(!config_params_all_required_set(&rec),
               "with the group bit unset, the board does not read back as fully commissioned");

    // Now stage the third channel and finalize again -- the group bit must
    // become set, proving this check can pass as well as fail.
    v.u8_val = 2u;
    TEST_CHECK(config_params_set(&rec, 0x0108u, KILNLINK_PARAM_TYPE_U8, v), "stage channel 2");
    config_params_finalize_ct_channel_map(&rec);
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_CT_CHANNEL_MAP) != 0,
               "all three channels staged: the group bit becomes set");

    // Monotonic: finalizing again after nothing new changes must not clear
    // an already-earned group bit.
    config_params_finalize_ct_channel_map(&rec);
    TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_CT_CHANNEL_MAP) != 0,
               "re-finalizing does not un-confirm an already-confirmed map");
}

static void test_config_params_all_required_set(void)
{
    TEST_SECTION("config_params_all_required_set -- every no-safe-default field, and only that set");

    config_store_record_t rec;
    config_store_default(&rec);
    TEST_CHECK(!config_params_all_required_set(&rec), "a fresh default record is not fully commissioned");

    // Stage every no-safe-default field except mains_voltage_v.
    kilnlink_param_value_t v;
    v.u8_val = CONFIG_STORE_TC_SOURCE_OWN_J7;
    config_params_set(&rec, 0x0101u, KILNLINK_PARAM_TYPE_U8, v); // tc_source
    v.u8_val = 0u;
    config_params_set(&rec, 0x0102u, KILNLINK_PARAM_TYPE_U8, v); // borrowed_zone_index
    v.u8_val = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    config_params_set(&rec, 0x0103u, KILNLINK_PARAM_TYPE_U8, v); // tc_placement_mode
    v.f32_val = 1300.0f;
    config_params_set(&rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v); // abs_max_temp_c
    v.u8_val = 0u;
    config_params_set(&rec, 0x0106u, KILNLINK_PARAM_TYPE_U8, v);
    v.u8_val = 1u;
    config_params_set(&rec, 0x0107u, KILNLINK_PARAM_TYPE_U8, v);
    v.u8_val = 2u;
    config_params_set(&rec, 0x0108u, KILNLINK_PARAM_TYPE_U8, v); // ct_channel_map, all 3
    v.f32_val = 5.0f;
    config_params_set(&rec, 0x0204u, KILNLINK_PARAM_TYPE_F32, v); // max_rate_c_per_min
    config_params_finalize_ct_channel_map(&rec);

    TEST_CHECK(!config_params_all_required_set(&rec),
               "mains_voltage_v still unset: NOT fully commissioned yet");

    v.f32_val = 240.0f;
    config_params_set(&rec, 0x030Eu, KILNLINK_PARAM_TYPE_F32, v); // mains_voltage_v
    TEST_CHECK(config_params_all_required_set(&rec),
               "every no-safe-default field now set: fully commissioned");
}

// GET_CONFIG_PAGE round trip: mirrors link_task_send_config_page()'s own
// logic (build entries from every id via config_params, pack pages, replay
// page_index from scratch each time) using only pure functions -- link_task.c
// itself is not host-tested (RTOS/uart_owner-dependent), but everything it
// calls to build this reply is, and this proves the composition actually
// round-trips a real record through every page.
static void test_config_params_get_config_page_roundtrip(void)
{
    TEST_SECTION("config_params + kilnlink_config_page -- GET_CONFIG_PAGE round trip");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.abs_max_temp_c = 1250.5f;
    rec.firing_margin_c = 88.0f;
    rec.watchdog_timeout_ms = 999u;
    rec.ct_cal[2].calibrated = true;
    rec.ct_cal[2].gain = 1.1f;

    size_t total = config_params_count();
    kilnlink_config_page_entry_t all[64];
    TEST_CHECK(total <= sizeof(all) / sizeof(all[0]), "id table fits the test's own scratch array");
    for (size_t i = 0; i < total; i++) {
        uint16_t id = 0;
        uint8_t type = 0;
        config_params_id_at(i, &id, &type);
        uint8_t got_type = 0;
        kilnlink_param_value_t value;
        memset(&value, 0, sizeof(value));
        TEST_CHECK(config_params_get(&rec, id, &got_type, &value), "every table id reads back from rec");
        all[i].param_id = id;
        all[i].type = got_type;
        all[i].value = value;
    }

    // Pack and decode every page, page_index 0, 1, 2, ... until more == 0,
    // collecting every (id, value) the wire actually carried.
    size_t offset = 0;
    uint8_t page_index = 0;
    size_t total_seen = 0;
    bool seen_abs_max_temp = false, seen_firing_margin = false, seen_watchdog = false, seen_ct_cal2 = false;
    for (;;) {
        uint8_t payload[KILNLINK_CONFIG_PAGE_HDR_LEN + KILNLINK_CONFIG_PAGE_ENTRY_MAX_LEN * 32u];
        size_t packed = 0;
        kilnlink_config_page_status_t pack_status;
        size_t len = kilnlink_config_page_pack(page_index, &all[offset], total - offset, payload,
                                                sizeof(payload), &packed, &pack_status);
        TEST_CHECK(len > 0, "each page packs successfully");
        offset += packed;

        kilnlink_config_page_t decoded;
        kilnlink_config_page_status_t decode_status = kilnlink_config_page_decode(payload, len, &decoded);
        TEST_CHECK(decode_status == KILNLINK_CONFIG_PAGE_OK, "each packed page decodes cleanly");
        TEST_CHECK(decoded.page_index == page_index, "decoded page_index echoes the request");

        for (size_t i = 0; i < decoded.entry_count; i++) {
            total_seen++;
            if (decoded.entries[i].param_id == 0x0104u) {
                seen_abs_max_temp = (decoded.entries[i].value.f32_val == 1250.5f);
            }
            if (decoded.entries[i].param_id == 0x0201u) {
                seen_firing_margin = (decoded.entries[i].value.f32_val == 88.0f);
            }
            if (decoded.entries[i].param_id == 0x0503u) {
                seen_watchdog = (decoded.entries[i].value.u16_val == 999u);
            }
            if (decoded.entries[i].param_id == 0x0318u) {
                seen_ct_cal2 = (decoded.entries[i].value.bool_val == 1u);
            }
        }

        if (!decoded.more) {
            break;
        }
        page_index++;
        TEST_CHECK(page_index < 32u, "the page loop terminates well within a sane bound"); // guards a runaway loop, never expected to fire
        if (page_index >= 32u) {
            break;
        }
    }

    TEST_CHECK(total_seen == total, "every id in the table was carried across exactly one page each");
    TEST_CHECK(seen_abs_max_temp, "abs_max_temp_c round-trips through GET_CONFIG_PAGE");
    TEST_CHECK(seen_firing_margin, "firing_margin_c round-trips through GET_CONFIG_PAGE");
    TEST_CHECK(seen_watchdog, "watchdog_timeout_ms round-trips through GET_CONFIG_PAGE");
    TEST_CHECK(seen_ct_cal2, "ct_cal[2].calibrated round-trips through GET_CONFIG_PAGE");
}

// Composed "commit while ARMED refused" check -- link_task_handle_commit_
// config() itself is not host-tested (config_store_write() needs a real
// RP2040, see config_store_flash.c's own header comment), but the pure
// pieces that decide the outcome are: a fully-valid staged record still
// must not be written while ARMED. This proves the composition a real
// commit performs -- validate() passing does not override
// config_store_decide_write()'s own unconditional ARMED refusal -- without
// needing flash at all.
static void test_config_params_commit_refused_while_armed(void)
{
    TEST_SECTION("commit_config composition -- ARMED refuses even a fully-valid staged record");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_source = CONFIG_STORE_TC_SOURCE_OWN_J7;
    rec.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    rec.fields_set |= (uint16_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_TC_PLACEMENT_MODE);

    const char *field = NULL;
    const char *rule = NULL;
    TEST_CHECK(config_params_validate(&rec, &field, &rule), "this staged record passes validation on its own");

    // The real link_task_handle_commit_config() calls config_store_write()
    // unconditionally after validate() passes -- config_store_write()'s own
    // internal config_store_decide_write() call is what refuses while
    // ARMED, exactly this decision:
    TEST_CHECK(config_store_decide_write(true) == CONFIG_STORE_WRITE_REFUSED_ARMED,
               "ARMED refuses the write regardless of validate()'s own outcome -- "
               "nothing this module computes can override the store's own ARMED gate");
    TEST_CHECK(config_store_decide_write(false) == CONFIG_STORE_WRITE_OK,
               "the same record, not ARMED, would be allowed to write -- proves the "
               "refusal above is really about ARMED, not some other property of the record");
}

// --- tc_type voltage-mode clamp (defense in depth, config_store.h's
// CONFIG_STORE_TC_TYPE_MAX_REAL) -------------------------------------------
//
// max31856_configure() (max31856.c, via max31856_tc_type_policy.h) is the
// primary enforcement point and already refuses tc_type > MAX31856_TC_TYPE_T
// outright. This is the second, independent layer: config_store_unpack()
// must never hand a CALLER (main.c, via config_store_get_tc_type()) a byte
// that would trip that refusal, even for a record whose CRC validates --
// e.g. a future writer that forgot to bound the field itself, or a record
// whose tc_type byte was corrupted in a way that happens to leave the CRC
// intact. Per this repo's "prove every new check can fail" rule, every one
// of the eight voltage-mode bytes is tested individually, in both the v2
// unpack path and the v1 migration path.
static void test_tc_type_voltage_mode_clamp_v2(void)
{
    TEST_SECTION("config_store_unpack (v2) -- tc_type voltage-mode clamp");

    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.format_version = CONFIG_STORE_FORMAT_VERSION;
    rec.seq = 1;
    rec.calibration_missing = true;

    // Accept direction: every real type (0x00-0x07) round-trips unchanged.
    for (uint8_t tc_type = 0x00u; tc_type <= 0x07u; tc_type++) {
        rec.tc_type = tc_type;
        uint8_t record[CONFIG_STORE_RECORD_LEN];
        config_store_pack(&rec, record);
        config_store_record_t back;
        bool ok = config_store_unpack(record, &back);
        TEST_CHECK(ok, "record with a real tc_type unpacks");
        TEST_CHECK(back.tc_type == tc_type, "real tc_type (0x00-0x07) passes through unchanged");
    }

    // Refuse direction: every voltage-mode byte (0x08-0x0F), individually,
    // must be clamped to CONFIG_STORE_DEFAULT_TC_TYPE, NOT passed through --
    // the record itself still unpacks (its CRC is valid), only the tc_type
    // field is substituted.
    uint8_t voltage_mode_bytes[8] = { 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu };
    for (size_t i = 0; i < 8u; i++) {
        // config_store_pack() writes rec->tc_type verbatim (it does no
        // validation of its own -- config_store.h's own doc comment) so this
        // hand-assembles exactly the CRC-valid-but-out-of-range record the
        // clamp exists to catch, standing in for "something upstream wrote
        // an out-of-range byte and this module is the last line of defense."
        rec.tc_type = voltage_mode_bytes[i];
        uint8_t record[CONFIG_STORE_RECORD_LEN];
        config_store_pack(&rec, record);
        config_store_record_t back;
        bool ok = config_store_unpack(record, &back);
        TEST_CHECK(ok, "a CRC-valid record with an out-of-range tc_type byte still unpacks "
                        "(the record itself is not corrupt)");
        TEST_CHECK(back.tc_type == CONFIG_STORE_DEFAULT_TC_TYPE,
                   "voltage-mode tc_type byte is clamped to CONFIG_STORE_DEFAULT_TC_TYPE, "
                   "never passed through to a caller");
    }

    // A couple of bytes outside the 4-bit field's own range too.
    uint8_t out_of_range_bytes[3] = { 0xFFu, 0x10u, 0x80u };
    for (size_t i = 0; i < 3u; i++) {
        rec.tc_type = out_of_range_bytes[i];
        uint8_t record[CONFIG_STORE_RECORD_LEN];
        config_store_pack(&rec, record);
        config_store_record_t back;
        bool ok = config_store_unpack(record, &back);
        TEST_CHECK(ok, "record unpacks even with a wildly out-of-range tc_type byte");
        TEST_CHECK(back.tc_type == CONFIG_STORE_DEFAULT_TC_TYPE,
                   "wildly out-of-range tc_type byte also clamped to the safe default");
    }
}

static void test_tc_type_voltage_mode_clamp_v1_migration(void)
{
    TEST_SECTION("config_store_unpack (v1 migration) -- tc_type voltage-mode clamp");

    config_store_ct_channel_cal_t ct_cal[3];
    memset(ct_cal, 0, sizeof(ct_cal));

    // Refuse direction on the v1 migration path too -- a legacy record
    // predates this bound existing at all, so it gets no less scrutiny.
    uint8_t voltage_mode_bytes[8] = { 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu };
    for (size_t i = 0; i < 8u; i++) {
        uint8_t v1_record[CONFIG_STORE_RECORD_LEN];
        pack_legacy_v1_record(1u, voltage_mode_bytes[i], true, ct_cal, v1_record);
        config_store_record_t out;
        bool ok = config_store_unpack(v1_record, &out);
        TEST_CHECK(ok, "a CRC-valid v1 record with an out-of-range tc_type byte still migrates");
        TEST_CHECK(out.tc_type == CONFIG_STORE_DEFAULT_TC_TYPE,
                   "migrated record's voltage-mode tc_type byte is clamped to the safe default, "
                   "not carried through from v1");
    }

    // Accept direction, for completeness: a real v1 tc_type still migrates
    // unchanged (already covered indirectly by test_v1_migration() above,
    // repeated here so this test file proves both directions on its own).
    uint8_t v1_record[CONFIG_STORE_RECORD_LEN];
    pack_legacy_v1_record(2u, 0x03u /* MAX31856_TC_TYPE_K */, true, ct_cal, v1_record);
    config_store_record_t out;
    bool ok = config_store_unpack(v1_record, &out);
    TEST_CHECK(ok, "a v1 record with a real tc_type migrates");
    TEST_CHECK(out.tc_type == 0x03u, "real v1 tc_type passes through the clamp unchanged");
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
    test_v2_full_roundtrip();
    test_v1_migration();
    test_future_version_refused();
    test_unset_fields_distinguishable_from_zero();
    test_tc_type_voltage_mode_clamp_v2();
    test_tc_type_voltage_mode_clamp_v1_migration();

    test_config_params_get_set_roundtrip();
    test_config_params_unknown_id_refused();
    test_config_params_type_mismatch_refused();
    test_config_params_set_range_validation();
    test_config_params_validate_range_validation();
    test_config_params_id_table_self_consistent();
    test_config_params_validate_contradiction_rejected();
    test_config_params_validate_ex_reason_and_id_lookup();
    test_config_params_ct_channel_map_two_of_three();
    test_config_params_all_required_set();
    test_config_params_get_config_page_roundtrip();
    test_config_params_commit_refused_while_armed();
}
