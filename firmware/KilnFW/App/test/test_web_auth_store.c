// Host tests for App/drivers/persist/web_auth_store.c -- credential storage
// foundation, docs/WEB_AUTH_PLAN.md sections 2, 3, 11. Standalone executable
// (own g_test_failures/g_test_count), same convention as test_crash_report.c:
// direct-#include of the module under test so its statics stay reachable,
// real round trips through fake_kv.h's RAM-backed hal_kv fake.
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "fake_kv.h"
#include "psa/crypto.h"

int g_test_failures = 0;
int g_test_count = 0;

// psa/crypto.h's host stub declares this `extern` (test_ota_http.c defines
// its own copy for its own executable) -- this standalone executable needs
// its own definition since it does not link test_ota_http.c.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "../drivers/persist/web_auth_store.c"

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL); // default `nvs` partition -- see this
                                  // module's own placement comment
}

static const uint8_t SALT_A[WEB_AUTH_SALT_LEN] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t SALT_B[WEB_AUTH_SALT_LEN] = {
    16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

// --- CRC32 sanity: the standard test vector, same one boot_guard.c's own
// construction is checked against elsewhere in this tree. ---------------
static void test_crc32_vector(void)
{
    TEST_SECTION("crc32_compute standard test vector");
    uint32_t crc = crc32_compute("123456789", 9);
    TEST_CHECK(crc == 0xCBF43926u, "CRC32(\"123456789\") must be the standard 0xCBF43926");
}

// --- Password strength (item 3) -----------------------------------------
static void test_password_strength(void)
{
    TEST_SECTION("web_auth_password_check");

    TEST_CHECK(web_auth_password_check("short1X", NULL, NULL, NULL) == WEB_AUTH_PW_TOO_SHORT,
               "under 10 chars must be rejected");
    TEST_CHECK(web_auth_password_check("alllowercase", NULL, NULL, NULL) ==
                   WEB_AUTH_PW_ALL_LOWERCASE,
               "all-lowercase (even >=10 chars) must be rejected");
    TEST_CHECK(web_auth_password_check("GoodPass123", NULL, NULL, NULL) == WEB_AUTH_PW_OK,
               ">=10 chars with a non-lowercase char must pass");

    char too_long[70];
    memset(too_long, 'A', sizeof(too_long) - 1);
    too_long[0] = 'A';
    too_long[1] = 'b'; // keep it non-lowercase-only, isolate the length check
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_CHECK(web_auth_password_check(too_long, NULL, NULL, NULL) == WEB_AUTH_PW_TOO_LONG,
               "over 64 chars must be rejected");

    TEST_CHECK(web_auth_password_check("Password12", NULL, NULL, NULL) ==
                   WEB_AUTH_PW_REJECTED_COMMON,
               "the literal word 'password' (case-insensitive) must be rejected");
    TEST_CHECK(web_auth_password_check("KilnPass12", NULL, NULL, NULL) ==
                   WEB_AUTH_PW_REJECTED_COMMON,
               "the literal word 'kiln' (case-insensitive) must be rejected");
    TEST_CHECK(web_auth_password_check("adminUser1", "adminUser1", NULL, NULL) ==
                   WEB_AUTH_PW_REJECTED_COMMON,
               "password equal to the username must be rejected");
    // Fixture-only placeholder strings, never a real credential: these
    // exercise the AP-SSID/AP-password rejection branches with obviously
    // synthetic values (see this task's "never write a real Wi-Fi
    // credential anywhere" constraint).
    TEST_CHECK(web_auth_password_check("FixtureSsidXX", NULL, "FixtureSsidXX", NULL) ==
                   WEB_AUTH_PW_REJECTED_COMMON,
               "password equal to the AP SSID must be rejected");
    TEST_CHECK(web_auth_password_check("FixtureApPwXX", NULL, NULL, "FixtureApPwXX") ==
                   WEB_AUTH_PW_REJECTED_COMMON,
               "password equal to the AP password must be rejected");
}

// --- PIN strength (item 3) ----------------------------------------------
static void test_pin_strength(void)
{
    TEST_SECTION("web_auth_pin_check");

    TEST_CHECK(web_auth_pin_check("123", NULL) == WEB_AUTH_PIN_TOO_SHORT,
               "under 4 digits must be rejected");
    TEST_CHECK(web_auth_pin_check("123456789", NULL) == WEB_AUTH_PIN_TOO_LONG,
               "over 8 digits must be rejected");
    TEST_CHECK(web_auth_pin_check("12a4", NULL) == WEB_AUTH_PIN_NOT_DIGITS,
               "a non-digit character must be rejected");
    TEST_CHECK(web_auth_pin_check("1234", NULL) == WEB_AUTH_PIN_OK,
               "4 digits, nothing to collide with, must pass");
    TEST_CHECK(web_auth_pin_check("1234", "1234") == WEB_AUTH_PIN_SAME_AS_OTHER,
               "a PIN equal to the other role's PIN must be rejected");
    TEST_CHECK(web_auth_pin_check("4321", "1234") == WEB_AUTH_PIN_OK,
               "a differing PIN against an existing other-role PIN must pass");
}

// Tiny local memmem -- MSVC's CRT has no memmem(); avoids pulling in a
// dependency just for one assertion.
static const void *memmem_local(const void *haystack, size_t hlen, const void *needle,
                                 size_t nlen)
{
    if (nlen == 0 || nlen > hlen) {
        return NULL;
    }
    const uint8_t *h = (const uint8_t *)haystack;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(h + i, needle, nlen) == 0) {
            return h + i;
        }
    }
    return NULL;
}

// --- Hashing: determinism, salt sensitivity, uniqueness (item 2) --------
static void test_hash_properties(void)
{
    TEST_SECTION("web_auth_hash_compute properties");

    uint8_t h1[WEB_AUTH_HASH_LEN], h2[WEB_AUTH_HASH_LEN], h3[WEB_AUTH_HASH_LEN];
    const char *pw = "SamePassword1";

    web_auth_hash_compute((const uint8_t *)pw, strlen(pw), SALT_A, 100, h1);
    web_auth_hash_compute((const uint8_t *)pw, strlen(pw), SALT_A, 100, h2);
    TEST_CHECK(web_auth_constant_time_equal(h1, h2, WEB_AUTH_HASH_LEN),
               "same secret+salt+iterations must hash identically");

    web_auth_hash_compute((const uint8_t *)pw, strlen(pw), SALT_B, 100, h3);
    TEST_CHECK(!web_auth_constant_time_equal(h1, h3, WEB_AUTH_HASH_LEN),
               "a different salt must produce a different hash");

    // Plaintext must never appear verbatim inside the derived hash bytes.
    TEST_CHECK(memmem_local(h1, WEB_AUTH_HASH_LEN, pw, strlen(pw)) == NULL,
               "the plaintext secret must not appear inside its own hash output");
}

// --- Set/verify round trip, correct and wrong password (item 2) --------
static void test_set_verify_password(void)
{
    TEST_SECTION("web_auth_store_set_password / verify_password");
    reset_all();

    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_USER) == false,
               "role with nothing set must report not-configured");

    hal_status_t st = web_auth_store_set_password(WEB_AUTH_ROLE_USER, "operator",
                                                   "CorrectHorse1", SALT_A, false);
    TEST_CHECK(st == HAL_OK, "set_password on a fresh store must succeed");
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_USER) == true,
               "role must now report configured");

    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "CorrectHorse1") == true,
               "the correct password must verify");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "WrongPassword1") == false,
               "an incorrect password must be rejected");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "") == false,
               "an empty password must be rejected");

    // The other role must be untouched by setting this one.
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false,
               "setting USER's password must not configure ADMINISTRATOR");

    // Plaintext must never appear in the persisted blob.
    web_auth_web_blob_t blob;
    hal_status_t rberr = load_blob(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    TEST_CHECK(rberr == HAL_OK, "the just-written blob must be readable back");
    TEST_CHECK(memmem_local(&blob, sizeof(blob), "CorrectHorse1", strlen("CorrectHorse1")) ==
                   NULL,
               "the plaintext password must never appear in the persisted blob");
}

static void test_set_verify_pin(void)
{
    TEST_SECTION("web_auth_store_set_pin / verify_pin");
    reset_all();

    hal_status_t st = web_auth_store_set_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "13579", SALT_B);
    TEST_CHECK(st == HAL_OK, "set_pin on a fresh store must succeed");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "13579") == true,
               "the correct PIN must verify");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "99999") == false,
               "an incorrect PIN must be rejected");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, "13579") == false,
               "a PIN set for one role must not verify for the other");
}

// --- Item 11: absent policy reads as both-off ---------------------------
static void test_policy_absent_is_off(void)
{
    TEST_SECTION("absent auth_policy record reads as both-off");
    reset_all();

    web_auth_policy_t pol;
    web_auth_load_status_t status = web_auth_store_load_policy(&pol);
    TEST_CHECK(status == WEB_AUTH_LOAD_ABSENT, "never-written policy must load ABSENT");
    TEST_CHECK(pol.web_enabled == false && pol.lcd_enabled == false,
               "an absent policy record's zeroed-out fields must read as both interfaces off");
    TEST_CHECK(web_auth_policy_effective_enabled(status, pol.web_enabled) == false,
               "effective_enabled(ABSENT) must collapse to false regardless of stored_enabled");
}

static void test_policy_set_and_load(void)
{
    TEST_SECTION("auth_policy set/load round trip");
    reset_all();

    web_auth_policy_t pol = {
        .web_enabled = true,
        .lcd_enabled = false,
        .web_timeout_s = 900,
        .lcd_timeout_s = -1,
    };
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "set_policy must succeed");

    web_auth_policy_t out;
    web_auth_load_status_t status = web_auth_store_load_policy(&out);
    TEST_CHECK(status == WEB_AUTH_LOAD_OK, "a freshly-written policy must load OK");
    TEST_CHECK(out.web_enabled == true && out.lcd_enabled == false && out.web_timeout_s == 900 &&
                   out.lcd_timeout_s == -1,
               "loaded policy fields must match what was set");
    TEST_CHECK(web_auth_policy_effective_enabled(status, out.web_enabled) == true,
               "effective_enabled(OK, true) must be true");
}

// --- Item 12b: a record too new for this build must fail closed, never
// collapse to "no credentials set" -- the OTA-rollback-past-a-schema-bump
// case named explicitly in this task. -------------------------------------
static void test_upgrade_path_unreadable_fails_closed(void)
{
    TEST_SECTION("version-too-new policy record fails closed (item 12b upgrade path)");
    reset_all();

    // Write a policy blob with web_enabled=true so a real board would have
    // logged in before an OTA rollback landed an older build here.
    web_auth_policy_t pol = {.web_enabled = true, .lcd_enabled = true};
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "seed a policy record");

    // Simulate "a future build wrote a newer schema this build cannot
    // parse": overwrite just the version field directly through the fake,
    // bypassing this module's own setter (which always stamps its own
    // current version) -- this is the only way to construct the rollback
    // scenario from a host test.
    web_auth_policy_blob_t blob;
    TEST_CHECK(load_blob(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob)) == HAL_OK,
               "must be able to read back the seeded blob to mutate it");
    blob.version = WEB_AUTH_STORE_VERSION + 1;
    blob.crc32 = policy_blob_crc(&blob); // keep the CRC self-consistent; only
                                          // the version is "from the future"
    TEST_CHECK(set_blob_verified(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob)) == HAL_OK,
               "must be able to persist the mutated blob");

    web_auth_policy_t out;
    web_auth_load_status_t status = web_auth_store_load_policy(&out);
    TEST_CHECK(status == WEB_AUTH_LOAD_UNREADABLE,
               "a version newer than this build knows must load UNREADABLE, not ABSENT and not OK");
    TEST_CHECK(web_auth_policy_effective_enabled(status, out.web_enabled) == true,
               "UNREADABLE must collapse to effective-enabled=true (fail closed, refuse login) "
               "rather than false (which would read as no auth required)");

    // And the credential record underneath must also refuse to authenticate
    // once its own blob is similarly unreadable -- fail closed end to end.
    web_auth_web_blob_t wblob;
    memset(&wblob, 0, sizeof(wblob));
    wblob.version = WEB_AUTH_STORE_VERSION + 1;
    wblob.crc32 = web_blob_crc(&wblob);
    TEST_CHECK(set_blob_verified(WEB_AUTH_KEY_WEB, &wblob, sizeof(wblob)) == HAL_OK,
               "seed an unreadable (future-versioned) web_auth blob");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "AnyPassword1") ==
                   false,
               "verify_password against an UNREADABLE record must always report false");
}

// --- The write-back-verification path itself: a write that lies about
// succeeding must be caught, not trusted -- see boot_guard_mark_healthy()'s
// history and this module's own set_blob_verified(). ----------------------
static void test_set_detects_lying_write(void)
{
    TEST_SECTION("set_password detects a write that reports success but changes nothing");
    reset_all();

    fake_kv_script_silent_set_noops(1);
    hal_status_t st = web_auth_store_set_password(WEB_AUTH_ROLE_USER, "operator",
                                                   "FirstPassword1", SALT_A, false);
    TEST_CHECK(st != HAL_OK,
               "a silently-noop'd write must be reported as a failure via read-back "
               "verification, not swallowed as success");
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_USER) == false,
               "the role must still read as not-configured after the lying write was caught");
}

// --- Item 11: enabling refused without an existing credential -- this
// module's contribution is the primitive (*_configured()); the actual
// refusal gate lives at the password-page call site (out of scope here),
// so this test proves the primitive itself is correct and would let that
// gate work. --------------------------------------------------------------
static void test_enable_gate_primitive(void)
{
    TEST_SECTION("password_configured()/pin_configured() as the enable-gate primitive");
    reset_all();

    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false,
               "no credential yet -- a caller's enable-gate must see false here");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", "AdminPass123",
                                            SALT_A, false) == HAL_OK,
               "set the administrator credential");
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == true,
               "now the enable-gate primitive must see true");
}

int main(void)
{
    test_crc32_vector();
    test_password_strength();
    test_pin_strength();
    test_hash_properties();
    test_set_verify_password();
    test_set_verify_pin();
    test_policy_absent_is_off();
    test_policy_set_and_load();
    test_upgrade_path_unreadable_fails_closed();
    test_set_detects_lying_write();
    test_enable_gate_primitive();

    fake_kv_reset_all();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
