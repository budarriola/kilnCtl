// Host tests for App/drivers/persist/totp_config.c -- NVS persistence,
// tri-state load status, and the RAM replay-counter cache. Joins the main
// combined host-test executable (fake_kv.c/hal_status.c already linked
// there for other NVS-backed modules); calls only totp_config.h's public
// API, no direct #include of the .c file (no static internals this test
// needs to reach).
#include <stdio.h>
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
    uint32_t ctr = 123;
    TEST_CHECK(totp_config_load_last_counter(&ctr) == TOTP_CONFIG_LOAD_ABSENT && ctr == 0,
               "replay counter reports ABSENT and defaults to 0 (never accepted)");
    ctr = 123;
    TEST_CHECK(totp_config_ram_last_counter(&ctr) && ctr == 0, "RAM cache also defaults to 0 on first read");
}

static uint32_t persisted_counter(void)
{
    uint32_t ctr = 0xDEADBEEFu;
    totp_config_load_status_t st = totp_config_load_last_counter(&ctr);
    return (st == TOTP_CONFIG_LOAD_UNREADABLE) ? 0xDEADBEEFu : ctr;
}

static uint32_t ram_counter(void)
{
    uint32_t ctr = 0xDEADBEEFu;
    return totp_config_ram_last_counter(&ctr) ? ctr : 0xDEADBEEFu;
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
    TEST_CHECK(persisted_counter() == 0, "a freshly-set secret resets the persisted counter to 0");
}

static void test_reenroll_resets_counter(void)
{
    TEST_SECTION("totp_config_set_secret -- re-enrollment resets a nonzero counter");
    reset_all();

    uint8_t secret1[TOTP_SECRET_LEN];
    memset(secret1, 0xAA, sizeof(secret1));
    TEST_CHECK(totp_config_set_secret(secret1), "first enrollment succeeds");
    TEST_CHECK(totp_config_set_last_counter(12345), "simulate some codes having been accepted");
    TEST_CHECK(persisted_counter() == 12345, "counter persisted as expected");

    uint8_t secret2[TOTP_SECRET_LEN];
    memset(secret2, 0xBB, sizeof(secret2));
    TEST_CHECK(totp_config_set_secret(secret2), "re-enrollment with a new secret succeeds");
    TEST_CHECK(persisted_counter() == 0,
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
    TEST_CHECK(ram_counter() == 500, "RAM cache lazy-loads the persisted counter on first call");

    // A direct set updates the RAM cache immediately, no reload needed.
    TEST_CHECK(totp_config_set_last_counter(501), "advance the counter");
    TEST_CHECK(ram_counter() == 501, "RAM cache reflects the update without a reload");
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
    uint32_t ctr = 1;
    TEST_CHECK(totp_config_load_last_counter(&ctr) == TOTP_CONFIG_LOAD_ABSENT && ctr == 0,
               "counter reads back ABSENT (0 sentinel) after clear");
    TEST_CHECK(ram_counter() == 0, "RAM cache is also cleared, not left stale");
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

static void test_read_error_is_unreadable_not_absent(void)
{
    TEST_SECTION("totp_config -- a read error or wrong-size blob is UNREADABLE, never ABSENT");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x44, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");
    TEST_CHECK(fake_kv_script_corrupt_key(NULL, "kiln_auth", "totp_secret"),
               "inject a read error on the committed secret");
    uint8_t discard[TOTP_SECRET_LEN];
    TEST_CHECK(totp_config_load_secret(discard) == TOTP_CONFIG_LOAD_UNREADABLE,
               "an NVS read error on a present secret reports UNREADABLE, not ABSENT");

    // Wrong-size secret blob (e.g. a future/older layout): UNREADABLE.
    reset_all();
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "open namespace");
    uint8_t short_blob[8] = {1, 0, 0, 0, 9, 9, 9, 9};
    TEST_CHECK(hal_kv_set_blob(&h, "totp_secret", short_blob, sizeof(short_blob)) == HAL_OK,
               "write a short secret blob");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(totp_config_load_secret(discard) == TOTP_CONFIG_LOAD_UNREADABLE,
               "a short secret blob reports UNREADABLE, not ABSENT");
}

static void test_unreadable_counter_fails_closed(void)
{
    TEST_SECTION("totp_config -- an unreadable replay counter never collapses to 0");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x66, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");
    TEST_CHECK(totp_config_set_last_counter(777), "persist counter 777");
    totp_config_ram_reset(); // simulate a reboot: RAM cache empty

    TEST_CHECK(fake_kv_script_corrupt_key(NULL, "kiln_auth", "totp_last_ctr"),
               "inject a read error on the committed counter");
    uint32_t ctr = 42;
    TEST_CHECK(totp_config_load_last_counter(&ctr) == TOTP_CONFIG_LOAD_UNREADABLE && ctr == 42,
               "load reports UNREADABLE and leaves *out untouched (never 0)");
    ctr = 42;
    TEST_CHECK(!totp_config_ram_last_counter(&ctr) && ctr == 42,
               "RAM fast path refuses (false) instead of serving 0 -- the caller must reject the code");

    // Once the counter is rewritten the fast path recovers (nothing cached from the failed read).
    TEST_CHECK(totp_config_set_last_counter(778), "rewrite the counter");
    TEST_CHECK(ram_counter() == 778, "RAM fast path serves the rewritten counter");
}

static void test_clear_keeps_counter_when_secret_erase_fails(void)
{
    TEST_SECTION("totp_config_clear -- a failed secret erase leaves the replay counter in place");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x21, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");
    TEST_CHECK(totp_config_set_last_counter(4242), "persist counter 4242");

    fake_kv_script_next_write_status(HAL_IO);
    TEST_CHECK(!totp_config_clear(), "clear reports failure when the secret erase fails");
    TEST_CHECK(totp_config_enrolled(), "secret still enrolled");
    TEST_CHECK(persisted_counter() == 4242,
               "counter NOT erased -- a surviving secret never gets a reset replay guard");
}

// Local copy of the zlib CRC32 so a test can forge a blob with a VALID CRC
// but a different version -- proving the version check itself, not the CRC,
// is what refuses it.
static uint32_t test_crc32(const uint8_t *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void raw_set(const char *key, const void *buf, size_t len)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "open namespace for injection");
    TEST_CHECK(hal_kv_set_blob(&h, key, buf, len) == HAL_OK, "inject raw blob");
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void test_wrong_version_and_oversize_are_unreadable(void)
{
    TEST_SECTION("totp_config -- wrong version (valid CRC) / oversize / truncated counter are UNREADABLE");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x5A, sizeof(secret));
    TEST_CHECK(totp_config_set_secret(secret), "enroll a secret");

    hal_kv_handle_t h;
    uint8_t blob[64];
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_ONLY, NULL) == HAL_OK, "open namespace");
    TEST_CHECK(hal_kv_get_blob(&h, "totp_secret", blob, &len) == HAL_OK && len == 32, "raw blob is 32 bytes");
    hal_kv_close(&h);

    blob[0] = 2; // version 2 (little-endian low byte)
    uint32_t crc = test_crc32(blob, 28);
    memcpy(blob + 28, &crc, sizeof(crc));
    raw_set("totp_secret", blob, 32);
    uint8_t discard[TOTP_SECRET_LEN];
    TEST_CHECK(totp_config_load_secret(discard) == TOTP_CONFIG_LOAD_UNREADABLE,
               "unknown version with a valid CRC is UNREADABLE, never ABSENT");

    uint8_t big[40];
    memset(big, 0, sizeof(big));
    raw_set("totp_secret", big, sizeof(big));
    TEST_CHECK(totp_config_load_secret(discard) == TOTP_CONFIG_LOAD_UNREADABLE,
               "an oversize secret blob is UNREADABLE, never ABSENT");

    uint8_t short_ctr[2] = {7, 0};
    raw_set("totp_last_ctr", short_ctr, sizeof(short_ctr));
    totp_config_ram_reset();
    uint32_t ctr = 42;
    TEST_CHECK(totp_config_load_last_counter(&ctr) == TOTP_CONFIG_LOAD_UNREADABLE && ctr == 42,
               "a truncated counter blob is UNREADABLE and leaves *out untouched");
    TEST_CHECK(!totp_config_ram_last_counter(&ctr), "RAM fast path refuses a truncated counter");
}

static void code_for(const uint8_t *secret, uint64_t counter, char out[8])
{
    snprintf(out, 8, "%06u", (unsigned)totp_hotp_code(secret, TOTP_SECRET_LEN, counter));
}

static void test_verify_and_consume(void)
{
    TEST_SECTION("totp_config_verify_and_consume -- replay guard persisted before success");
    reset_all();

    uint8_t secret[TOTP_SECRET_LEN];
    for (unsigned i = 0; i < TOTP_SECRET_LEN; i++) secret[i] = (uint8_t)(0xC0 + i);
    const uint64_t now = 1700000000;
    const uint64_t n = totp_counter_for_time(now);
    char code[8];

    code_for(secret, n, code);
    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_NOT_ENROLLED,
               "no secret -> NOT_ENROLLED");

    TEST_CHECK(totp_config_set_secret(secret), "enroll");
    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_OK, "step N accepted");
    TEST_CHECK(persisted_counter() == (uint32_t)n, "step N is in NVS by the time OK is returned");

    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_REJECTED,
               "second call with the same code is refused");
    totp_config_ram_reset(); // simulated reboot: the guard must come back from NVS
    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_REJECTED,
               "same code still refused after a reboot (NVS-backed guard)");
    code_for(secret, n - 1, code);
    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_REJECTED,
               "step N-1 refused after N was accepted");
    code_for(secret, n + 1, code);
    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_OK, "step N+1 accepted");
    TEST_CHECK(persisted_counter() == (uint32_t)(n + 1), "counter advanced to N+1");
    code_for(secret, n, code);
    TEST_CHECK(totp_config_verify_and_consume(code, now) == TOTP_CONSUME_REJECTED,
               "step N refused once N+1 was accepted");

    TEST_CHECK(totp_config_verify_and_consume("12345", now) == TOTP_CONSUME_REJECTED,
               "malformed code -> REJECTED");

    // Persist failure: never OK.
    code_for(secret, n + 2, code);
    fake_kv_script_next_write_status(HAL_IO);
    TEST_CHECK(totp_config_verify_and_consume(code, now + 30) == TOTP_CONSUME_UNAVAILABLE,
               "a failed counter write is UNAVAILABLE, never OK");
    TEST_CHECK(persisted_counter() == (uint32_t)(n + 1), "counter unchanged after the failed write");

    // Unreadable counter: refuse even a valid fresh code.
    TEST_CHECK(fake_kv_script_corrupt_key(NULL, "kiln_auth", "totp_last_ctr"), "corrupt the counter");
    totp_config_ram_reset();
    TEST_CHECK(totp_config_verify_and_consume(code, now + 30) == TOTP_CONSUME_UNAVAILABLE,
               "unreadable counter -> UNAVAILABLE even for a valid code");

    // Unreadable secret: UNAVAILABLE, not NOT_ENROLLED.
    reset_all();
    TEST_CHECK(totp_config_set_secret(secret), "re-enroll");
    TEST_CHECK(fake_kv_script_corrupt_key(NULL, "kiln_auth", "totp_secret"), "corrupt the secret");
    TEST_CHECK(totp_config_verify_and_consume(code, now + 30) == TOTP_CONSUME_UNAVAILABLE,
               "unreadable secret -> UNAVAILABLE, never NOT_ENROLLED");
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
    test_read_error_is_unreadable_not_absent();
    test_unreadable_counter_fails_closed();
    test_clear_keeps_counter_when_secret_erase_fails();
    test_wrong_version_and_oversize_are_unreadable();
    test_verify_and_consume();
}
