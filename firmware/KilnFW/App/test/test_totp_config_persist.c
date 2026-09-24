// Host tests for App/drivers/persist/totp_config.c -- NVS persistence,
// tri-state load status, and the RAM replay-counter cache. Joins the main
// combined host-test executable (fake_kv.c/hal_status.c already linked
// there for other NVS-backed modules); calls only totp_config.h's public
// API, no direct #include of the .c file (no static internals this test
// needs to reach).
#include <string.h>

#include "test_common.h"

#include "fake_kv.h"
#include "hal_kv.h"
#include "../drivers/persist/totp_config.h"

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL); // default `nvs` partition, same as totp_config.c uses
    totp_config_ram_reset();
}

static void test_absent_by_default(void)
{
    TEST_SECTION("totp_config -- absent on a never-enrolled store");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    TEST_CHECK(totp_config_load_secret(secret) == TOTP_CONFIG_LOAD_ABSENT,
               "a never-written store reports ABSENT, not UNREADABLE or a stale OK");
    TEST_CHECK(!totp_config_enrolled(), "totp_config_enrolled() is false before any enrollment");
    TEST_CHECK(totp_config_load_last_counter() == 0, "replay counter defaults to 0 (never accepted)");
    TEST_CHECK(totp_config_ram_last_counter() == 0, "RAM cache also defaults to 0 on first read");
}

static void test_set_and_load_round_trip(void)
{
    TEST_SECTION("totp_config_set_secret / totp_config_load_secret -- round trip");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    for (unsigned i = 0; i < TOTP_SECRET_LEN; i++) secret[i] = (uint8_t)(i * 7 + 3);

    TEST_CHECK(totp_config_set_secret(secret), "set_secret reports success");
    TEST_CHECK(totp_config_enrolled(), "enrolled() is true immediately after set_secret");

    uint8_t loaded[TOTP_SECRET_LEN];
    TEST_CHECK(totp_config_load_secret(loaded) == TOTP_CONFIG_LOAD_OK, "load reports OK");
    TEST_CHECK(memcmp(loaded, secret, TOTP_SECRET_LEN) == 0, "loaded secret matches what was set");

    // A fresh enrollment resets the replay counter to 0, even if this were
    // a re-enrollment over an existing one (checked explicitly below).
    TEST_CHECK(totp_config_load_last_counter() == 0, "a freshly-set secret resets the persisted counter to 0");
}

static void test_reenroll_resets_counter(void)
{
    TEST_SECTION("totp_config_set_secret -- re-enrollment resets a nonzero counter");
    reset_all();

    uint8_t secret1[TOTP_SECRET_LEN];
    memset(secret1, 0xAA, sizeof(secret1));
    TEST_CHECK(totp_config_set_secret(secret1), "first enrollment succeeds");
    TEST_CHECK(totp_config_set_last_counter(12345), "simulate some codes having been accepted");
    TEST_CHECK(totp_config_load_last_counter() == 12345, "counter persisted as expected");

    uint8_t secret2[TOTP_SECRET_LEN];
    memset(secret2, 0xBB, sizeof(secret2));
    TEST_CHECK(totp_config_set_secret(secret2), "re-enrollment with a new secret succeeds");
    TEST_CHECK(totp_config_load_last_counter() == 0,
               "re-enrollment resets the counter -- a stale counter from the OLD secret must "
               "never be inherited by the new one");

    uint8_t loaded[TOTP_SECRET_LEN];
    totp_config_load_secret(loaded);
    TEST_CHECK(memcmp(loaded, secret2, TOTP_SECRET_LEN) == 0, "the NEW secret is the one now stored");
}

static void test_ram_cache_lazy_load_and_update(void)
{
    TEST_SECTION("totp_config_ram_last_counter -- lazy load, then in-RAM fast path");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x55, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");
    TEST_CHECK(totp_config_set_last_counter(500), "persist counter 500");

    // Force the RAM cache to look "not yet loaded this boot" by resetting it
    // without touching NVS, then confirm it lazy-loads the persisted value.
    totp_config_ram_reset();
    TEST_CHECK(totp_config_ram_last_counter() == 500, "RAM cache lazy-loads the persisted counter on first call");

    // A direct set updates the RAM cache immediately, no reload needed.
    TEST_CHECK(totp_config_set_last_counter(501), "advance the counter");
    TEST_CHECK(totp_config_ram_last_counter() == 501, "RAM cache reflects the update without a reload");
}

static void test_clear_erases_both_keys(void)
{
    TEST_SECTION("totp_config_clear -- erases secret and counter, read-back verified");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x77, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");
    TEST_CHECK(totp_config_set_last_counter(999), "persist a nonzero counter");

    TEST_CHECK(totp_config_clear(), "clear reports success");
    TEST_CHECK(!totp_config_enrolled(), "no longer enrolled after clear");

    uint8_t discard[TOTP_SECRET_LEN];
    TEST_CHECK(totp_config_load_secret(discard) == TOTP_CONFIG_LOAD_ABSENT,
               "secret reads back ABSENT after clear");
    TEST_CHECK(totp_config_load_last_counter() == 0, "counter reads back to the 0 sentinel after clear");
    TEST_CHECK(totp_config_ram_last_counter() == 0, "RAM cache is also cleared, not left stale");
}

static void test_clear_is_idempotent_on_never_enrolled(void)
{
    TEST_SECTION("totp_config_clear -- succeeds even when nothing was ever enrolled");
    reset_all();

    TEST_CHECK(totp_config_clear(), "clearing a never-written store still reports success");
    TEST_CHECK(!totp_config_enrolled(), "still not enrolled");
}

static void test_unreadable_on_corrupt_blob(void)
{
    TEST_SECTION("totp_config -- a CRC-corrupted blob reports UNREADABLE, not ABSENT");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x33, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");

    // Corrupt the persisted blob directly via the fake KV backend, bypassing
    // totp_config.c's own setters -- simulates flash bit-rot / a partial
    // write, same technique test_web_auth_store.c/test_boot_guard.c use for
    // their own UNREADABLE-path coverage.
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK,
               "can reopen the namespace directly for corruption injection");
    // Buffer sized generously -- the real on-disk blob may be padded past
    // the naive field-sum (struct alignment before the trailing crc32), so
    // this must not assume a packed layout; hal_kv_get_blob reports the
    // actual stored length back through len.
    uint8_t blob[64];
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, "totp_secret", blob, &len) == HAL_OK, "read the raw blob back");
    blob[len - 1] ^= 0xFF; // flip bits in the trailing crc32 field
    TEST_CHECK(hal_kv_set_blob(&h, "totp_secret", blob, len) == HAL_OK, "write the corrupted blob back");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    uint8_t discard[TOTP_SECRET_LEN];
    TEST_CHECK(totp_config_load_secret(discard) == TOTP_CONFIG_LOAD_UNREADABLE,
               "a CRC mismatch reports UNREADABLE -- never silently reinterpreted, never "
               "collapsed to ABSENT (which would look like 'never enrolled', not 'corrupt')");
    TEST_CHECK(!totp_config_enrolled(), "enrolled() is false (fail closed) for an UNREADABLE blob too");
}

void run_test_totp_config_persist(void)
{
    test_absent_by_default();
    test_set_and_load_round_trip();
    test_reenroll_resets_counter();
    test_ram_cache_lazy_load_and_update();
    test_clear_erases_both_keys();
    test_clear_is_idempotent_on_never_enrolled();
    test_unreadable_on_corrupt_blob();
}
