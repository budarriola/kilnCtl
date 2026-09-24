// Host tests for App/drivers/persist/ota_record.c -- specifically ota_record_fill(),
// the pure (no ESP-IDF I/O) half of the module. Added 2026-08-21 alongside
// ota_record_t's new image_sha256_hex field (CommonFW/docs/UPDATE_PROTOCOL.md's
// "an append-only update record in NVS: timestamp, processor, image SHA-256,
// version before and after, result").
//
// ota_record_append()/ota_record_load() (the NVS I/O, now hal_kv_* per
// HW_ABSTRACTION.md Phase 3 item 3) are NOT exercised here -- this
// file's own header comment already explains why the module as a whole
// isn't host-tested (run_state.c/relay_cycles.c precedent), and this file
// never #includes ota_record.c or opens a real (fake_kv-backed) partition,
// so there would be nothing real to assert about a round trip anyway. What
// IS worth a host test, now that this field exists,
// is the pure fill/truncate/NULL-tolerance behavior every other string field
// in ota_record_t already gets implicitly exercised by -- this file makes
// that explicit for the new field specifically.
#include <string.h>

#include "test_common.h"

#include "../drivers/persist/ota_record.h"

static void test_fill_populates_sha256(void)
{
    TEST_SECTION("ota_record_fill -- image_sha256_hex populated verbatim when it fits");

    ota_record_t rec;
    const char *hash64 =
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"; // 4 * 16 = 64 hex chars
    TEST_CHECK(strlen(hash64) == 64, "test fixture itself must be exactly 64 chars");

    ota_record_fill(&rec, 123u, "esp", "1.0.0", "1.1.0", true, "ok", hash64);

    TEST_CHECK(strcmp(rec.image_sha256_hex, hash64) == 0,
               "a 64-char hex hash fits OTA_RECORD_SHA256_HEX_MAX-1 and is stored verbatim");
    TEST_CHECK(rec.version == OTA_RECORD_VERSION, "version field still stamped");
    TEST_CHECK(strcmp(rec.processor, "esp") == 0, "processor field unaffected by the new field");
}

static void test_fill_null_hash_is_empty_string(void)
{
    TEST_SECTION("ota_record_fill -- NULL image_sha256_hex_or_null yields \"\", never a placeholder");

    ota_record_t rec;
    memset(&rec, 0xAA, sizeof(rec)); // poison first, so a leftover byte would be visible

    ota_record_fill(&rec, 0u, "pico", "", "", false, "some failure", NULL);

    TEST_CHECK(rec.image_sha256_hex[0] == '\0',
               "NULL hash -- record's hash field is an empty string, not garbage or a fake value");
}

static void test_fill_truncates_oversized_hash(void)
{
    TEST_SECTION("ota_record_fill -- an oversized hash string is truncated, never overflows");

    // NEGATIVE-TEST PROOF, per this repo's "prove a new check can fail"
    // discipline: OTA_RECORD_SHA256_HEX_MAX is 65 (64 hex chars + NUL). Feed
    // ota_record_fill() a string one character LONGER than that and confirm
    // truncation actually happens (stored length == 64, not 65) -- if
    // copy_str()'s cap arithmetic were ever off by one, this is the check
    // that would catch it.
    char too_long[OTA_RECORD_SHA256_HEX_MAX + 1 + 1]; // 67 chars + NUL
    memset(too_long, 'f', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_CHECK(strlen(too_long) == OTA_RECORD_SHA256_HEX_MAX + 1,
               "test fixture is deliberately one char longer than the field can hold");

    ota_record_t rec;
    ota_record_fill(&rec, 0u, "esp", "", "", true, "ok", too_long);

    TEST_CHECK(strlen(rec.image_sha256_hex) == OTA_RECORD_SHA256_HEX_MAX - 1,
               "stored hash is truncated to exactly OTA_RECORD_SHA256_HEX_MAX-1 chars, not left "
               "at the input's full length");
    TEST_CHECK(rec.image_sha256_hex[OTA_RECORD_SHA256_HEX_MAX - 1] == '\0',
               "still NUL-terminated within the field -- no overflow into adjacent struct fields");
}

static void test_fill_does_not_clobber_other_fields(void)
{
    TEST_SECTION("ota_record_fill -- adjacent fields (reason, reserved padding) are untouched by "
                 "the new hash field");

    ota_record_t rec;
    ota_record_fill(&rec, 42u, "esp", "1.0.0", "2.0.0", true, "ok",
                     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    TEST_CHECK(strcmp(rec.reason, "ok") == 0, "reason field intact next to the new hash field");
    TEST_CHECK(rec.uptime_s == 42u, "uptime_s intact");
    TEST_CHECK(strcmp(rec.version_after, "2.0.0") == 0, "version_after intact");
}

// --- ota_version_compare() / ota_record_fill()'s is_downgrade wiring -------
// UPDATE_PROTOCOL.md's "a downgrade is allowed but logged as such" bullet
// (sections 4 and 6a). version strings in this codebase are ESP-IDF's
// `git describe`-shaped esp_app_desc_t.version default (e.g.
// "v1.2-15-gabc1234"), not a clean semver -- see ota_record.h's header
// comment on ota_version_compare() for the parsing rule this exercises.

static void test_compare_detects_downgrade(void)
{
    TEST_SECTION("ota_version_compare -- a lower version is OLDER (downgrade)");
    TEST_CHECK(ota_version_compare("v2.5-10-gabc1234", "v2.3-1-gdef5678") == OTA_VERSION_CMP_OLDER,
               "2.3 after 2.5 is a downgrade");
    TEST_CHECK(ota_version_compare("1.0.0", "0.9.0") == OTA_VERSION_CMP_OLDER,
               "plain dotted versions also compare");
}

static void test_compare_detects_upgrade_and_same(void)
{
    TEST_SECTION("ota_version_compare -- upgrade and identical versions are never OLDER");
    TEST_CHECK(ota_version_compare("v2.3-1-gdef5678", "v2.5-10-gabc1234") == OTA_VERSION_CMP_NEWER,
               "2.5 after 2.3 is an upgrade");
    TEST_CHECK(ota_version_compare("1.2.3", "1.2.3") == OTA_VERSION_CMP_SAME,
               "identical strings compare equal");
    // NEGATIVE-TEST-shaped check: the commit-count component (the 3rd
    // dash-separated field in a git-describe string) must actually be
    // compared, not just the leading dotted pair -- catches a comparator
    // that stops after two components.
    TEST_CHECK(ota_version_compare("v1.0-3-gaaaa", "v1.0-9-gbbbb") == OTA_VERSION_CMP_NEWER,
               "same major.minor, higher commit count is still newer");
}

static void test_compare_unknown_on_unparseable_or_empty(void)
{
    TEST_SECTION("ota_version_compare -- empty or non-numeric strings are UNKNOWN, never guessed");
    TEST_CHECK(ota_version_compare("", "1.0.0") == OTA_VERSION_CMP_UNKNOWN, "empty before -- unknown");
    TEST_CHECK(ota_version_compare("1.0.0", "") == OTA_VERSION_CMP_UNKNOWN, "empty after -- unknown");
    TEST_CHECK(ota_version_compare("", "") == OTA_VERSION_CMP_UNKNOWN, "both empty (the Pico path's case) -- unknown");
    TEST_CHECK(ota_version_compare("dirty", "also-dirty") == OTA_VERSION_CMP_UNKNOWN,
               "no leading digit anywhere -- unknown, not a false SAME");
}

// The shapes this tree really emits (review of 9ca22048): esp_app_desc_t.version
// is 31 chars max, so with the repo's 25-char tag the hash and "-dirty" are
// truncated off entirely. And where a hash IS visible, its digits must never
// order two builds -- the first comparator read "g9ca22048" as the number 9.
static void test_compare_real_describe_shapes(void)
{
    TEST_SECTION("ota_version_compare -- real truncated describe strings, hashes, dirty, bare hashes");
    TEST_CHECK(ota_version_compare("V1.0_Purchased_This_Board-3140-",
                                   "V1.0_Purchased_This_Board-3141-") == OTA_VERSION_CMP_NEWER,
               "bench shape: higher commit count is newer");
    TEST_CHECK(ota_version_compare("V1.0_Purchased_This_Board-3141-",
                                   "V1.0_Purchased_This_Board-3140-") == OTA_VERSION_CMP_OLDER,
               "bench shape: lower commit count is a downgrade");
    TEST_CHECK(ota_version_compare("V1.0_Purchased_This_Board-3140-",
                                   "V1.0_Purchased_This_Board-3140-") == OTA_VERSION_CMP_SAME,
               "bench shape: identical strings are the same");
    TEST_CHECK(ota_version_compare("v1.0-5-g9000000", "v1.0-5-g1234567") == OTA_VERSION_CMP_UNKNOWN,
               "same count, different commits -- unknown, never ordered by hash digits");
    TEST_CHECK(ota_version_compare("v1.0-5-g1234567", "v1.0-5-g9000000") == OTA_VERSION_CMP_UNKNOWN,
               "same count, different commits (reversed) -- unknown");
    TEST_CHECK(ota_version_compare("v1.2-15-gabc1234", "v1.2-15-gabc1234-dirty") == OTA_VERSION_CMP_SAME,
               "dirty flag alone does not order two builds of one commit");
    TEST_CHECK(ota_version_compare("v1.0-9-g0000001", "v1.0-10-g9999999") == OTA_VERSION_CMP_NEWER,
               "count decides, not hash digits");
    TEST_CHECK(ota_version_compare("v1.0", "v1.0-3-gabc1234") == OTA_VERSION_CMP_NEWER,
               "exact tag, then commits past it, is newer");
    TEST_CHECK(ota_version_compare("9ca22048", "42762cb0") == OTA_VERSION_CMP_UNKNOWN,
               "bare hashes (no tag reachable) -- unknown");
    TEST_CHECK(ota_version_compare("12345678-dirty", "42762cb0") == OTA_VERSION_CMP_UNKNOWN,
               "all-digit bare hash is still a hash -- unknown");
    TEST_CHECK(ota_version_compare("v1.0-7-g1234567-di", "v1.0-7-g1234567") == OTA_VERSION_CMP_SAME,
               "truncated -dirty suffix is stripped");
}

static void test_fill_sets_is_downgrade_for_a_real_downgrade(void)
{
    TEST_SECTION("ota_record_fill -- is_downgrade/version_compare_known wired from ota_version_compare");

    ota_record_t rec;
    ota_record_fill(&rec, 0u, "esp", "v2.5-10-gabc1234", "v2.3-1-gdef5678", true, "ok", NULL);

    TEST_CHECK(rec.version_compare_known == 1u, "both strings parseable -- comparison known");
    TEST_CHECK(rec.is_downgrade == 1u, "record correctly flags this as a downgrade");
}

static void test_fill_never_flags_an_upgrade_as_a_downgrade(void)
{
    TEST_SECTION("ota_record_fill -- an upgrade is never mislabeled a downgrade "
                 "(the negative-test proof for the field above)");

    ota_record_t rec;
    ota_record_fill(&rec, 0u, "esp", "v2.3-1-gdef5678", "v2.5-10-gabc1234", true, "ok", NULL);

    TEST_CHECK(rec.version_compare_known == 1u, "both strings parseable -- comparison known");
    TEST_CHECK(rec.is_downgrade == 0u, "an upgrade must never read back as a downgrade");
}

static void test_fill_is_downgrade_unknown_for_pico_blank_versions(void)
{
    TEST_SECTION("ota_record_fill -- the Pico path's blank version_before/version_after "
                 "(ota_pico_relay.c's `done:` label -- no trustworthy Pico version string exists "
                 "from this side) reads back as unknown, never a false is_downgrade=0 'answer'");

    ota_record_t rec;
    ota_record_fill(&rec, 0u, "pico", "", "", true, "ok", NULL);

    TEST_CHECK(rec.version_compare_known == 0u, "blank strings -- comparison not known");
    TEST_CHECK(rec.is_downgrade == 0u, "is_downgrade is the documented safe default, not a real answer");
}

void run_test_ota_record(void)
{
    test_fill_populates_sha256();
    test_fill_null_hash_is_empty_string();
    test_fill_truncates_oversized_hash();
    test_fill_does_not_clobber_other_fields();
    test_compare_detects_downgrade();
    test_compare_detects_upgrade_and_same();
    test_compare_unknown_on_unparseable_or_empty();
    test_compare_real_describe_shapes();
    test_fill_sets_is_downgrade_for_a_real_downgrade();
    test_fill_never_flags_an_upgrade_as_a_downgrade();
    test_fill_is_downgrade_unknown_for_pico_blank_versions();
}
