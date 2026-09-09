// Host tests for App/drivers/safety/estop_verification.c -- the persisted
// record of whether an operator has run the E-stop bench verification
// procedure (firmware/SaftyFW/README.md).
//
// estop_verification.c is #included directly (same convention as
// test_crash_report.c's #include of crash_report.c) so this file can reach
// its static load()/persist() helpers and exercise a genuine round trip
// through fake_kv.h's RAM-backed hal_kv fake.
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "../drivers/safety/estop_verification.c"

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(KILN_NVS_PARTITION);
}

static void test_unverified_by_default(void)
{
    TEST_SECTION("estop_verification -- reads unverified before any confirmation");
    reset_all();
    TEST_CHECK(estop_verification_is_verified() == false, "no record yet -- unverified");
}

static void test_confirm_sets_verified_and_survives_reload(void)
{
    TEST_SECTION("estop_verification -- confirm() persists, a fresh load sees it too");
    reset_all();
    esp_err_t err = estop_verification_confirm();
    TEST_CHECK(err == ESP_OK, "confirm() succeeds against a healthy fake_kv backend");
    TEST_CHECK(estop_verification_is_verified() == true, "verified immediately after confirm()");

    // A second, independent load call (not just re-reading a cached RAM
    // value) -- proves this is a real NVS round trip, same discipline
    // test_crash_report.c's "survives reload" tests use.
    estop_verif_record_t rec;
    memset(&rec, 0, sizeof(rec));
    TEST_CHECK(load(&rec) == true, "load() finds the persisted record");
    TEST_CHECK(rec.verified == 1u, "the persisted record itself says verified");
}

static void test_clear_reverts_to_unverified(void)
{
    TEST_SECTION("estop_verification -- clear() reverts a verified record to unverified");
    reset_all();
    TEST_CHECK(estop_verification_confirm() == ESP_OK, "confirmed first");
    TEST_CHECK(estop_verification_is_verified() == true, "verified before clear()");

    esp_err_t err = estop_verification_clear();
    TEST_CHECK(err == ESP_OK, "clear() succeeds");
    TEST_CHECK(estop_verification_is_verified() == false, "unverified after clear()");
}

static void test_clear_on_already_unverified_is_a_harmless_no_op(void)
{
    TEST_SECTION("estop_verification -- clear() with no standing record does not fail or fabricate one");
    reset_all();
    TEST_CHECK(estop_verification_is_verified() == false, "unverified before clear()");
    esp_err_t err = estop_verification_clear();
    TEST_CHECK(err == ESP_OK, "clearing an already-unverified record is not an error");
    TEST_CHECK(estop_verification_is_verified() == false, "still unverified after clearing nothing");
}

static void test_corrupt_version_reads_as_unverified(void)
{
    TEST_SECTION("estop_verification -- a record with an unrecognised version reads unverified, "
                 "never a stale true");
    reset_all();
    TEST_CHECK(estop_verification_confirm() == ESP_OK, "confirmed");
    TEST_CHECK(estop_verification_is_verified() == true, "verified before corruption");

    // Simulate a future/incompatible layout landing in this slot -- same
    // "version mismatch is treated as no record" fail-safe direction
    // crash_report.c's record_valid() uses.
    estop_verif_record_t bad;
    bad.version = ESTOP_VERIF_RECORD_VERSION + 1u;
    bad.verified = 1u;
    esp_err_t perr = persist(&bad);
    TEST_CHECK(perr == ESP_OK, "the malformed record itself writes fine -- corruption is about "
                              "meaning, not I/O");
    TEST_CHECK(estop_verification_is_verified() == false,
               "an unrecognised version reads unverified even though the stored byte says verified=1 -- "
               "fail-safe, not fail-open");
}

void run_test_estop_verification(void)
{
    test_unverified_by_default();
    test_confirm_sets_verified_and_survives_reload();
    test_clear_reverts_to_unverified();
    test_clear_on_already_unverified_is_a_harmless_no_op();
    test_corrupt_version_reads_as_unverified();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
