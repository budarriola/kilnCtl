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

// Direct-#include of the gesture state machine too (same convention as
// web_auth_store.c above) -- only this executable links both fake_kv.h and
// psa/crypto.h's host stub, so it is the one place the WIRED path (item
// 10's confirm step actually calling web_auth_store_clear_for_physical_
// reset()) can be exercised end-to-end. auth_reset_gesture.c itself has no
// I/O dependency and is otherwise tested standalone in
// test_auth_reset_gesture.c (the combined host-test executable); this file
// only adds the negative case that needs a real, wired, non-null seam:
// "a gesture that aborts partway must leave the stored credentials intact".
#include "../drivers/net/auth_reset_gesture.c"

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

// --- Item 11: enabling refused without an existing credential -- the
// underlying primitive (*_configured()) is what web_auth_policy_check_
// transition() below is built on. -----------------------------------------
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

// --- Item 11 acceptance: "enabling web auth with no administrator
// credential is refused; enabling clears all sessions and disabling does
// not." web_auth_policy_check_transition() is the one place this decision
// is made -- see its header comment for why it is not re-derived at the
// (out-of-scope-here) password-page call site. -----------------------------
static void test_policy_check_transition(void)
{
    TEST_SECTION("web_auth_policy_check_transition: enable gate + edge-triggered session clear");

    web_auth_policy_t off = {.web_enabled = false, .lcd_enabled = false};
    web_auth_policy_t web_on = {.web_enabled = true, .lcd_enabled = false};
    web_auth_policy_t lcd_on = {.web_enabled = false, .lcd_enabled = true};
    bool clear_web, clear_lcd;

    // Refused: turning web on with no administrator password configured.
    web_auth_policy_transition_t r = web_auth_policy_check_transition(
        &off, &web_on, /*admin_password_configured=*/false, /*admin_pin_configured=*/false,
        &clear_web, &clear_lcd);
    TEST_CHECK(r == WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_WEB_CREDENTIAL,
               "enabling web auth with no administrator credential must be refused");
    TEST_CHECK(clear_web == false && clear_lcd == false,
               "a refused transition must report no session clears at all");

    // Refused: turning lcd on with no administrator PIN configured.
    r = web_auth_policy_check_transition(&off, &lcd_on, /*admin_password_configured=*/true,
                                          /*admin_pin_configured=*/false, &clear_web, &clear_lcd);
    TEST_CHECK(r == WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_LCD_CREDENTIAL,
               "enabling lcd auth with no administrator PIN must be refused, independent of "
               "the web credential's state");

    // Allowed: turning web on (off -> on) with the credential present must
    // clear web sessions but never touch the (untouched) lcd session.
    r = web_auth_policy_check_transition(&off, &web_on, /*admin_password_configured=*/true,
                                          /*admin_pin_configured=*/false, &clear_web, &clear_lcd);
    TEST_CHECK(r == WEB_AUTH_POLICY_TRANSITION_OK, "enabling web with a credential must succeed");
    TEST_CHECK(clear_web == true && clear_lcd == false,
               "enabling web (off->on) must clear web sessions and only web sessions");

    // Allowed: re-saving an already-on policy (no edge) must NOT clear
    // sessions -- a caller changing only a timeout must not silently log
    // everyone out.
    r = web_auth_policy_check_transition(&web_on, &web_on, /*admin_password_configured=*/true,
                                          /*admin_pin_configured=*/false, &clear_web, &clear_lcd);
    TEST_CHECK(r == WEB_AUTH_POLICY_TRANSITION_OK, "re-saving an already-on policy must succeed");
    TEST_CHECK(clear_web == false,
               "re-saving an unchanged already-enabled policy must not clear sessions");

    // Allowed: disabling (on -> off) must never clear sessions ("sessions
    // become irrelevant but are kept").
    r = web_auth_policy_check_transition(&web_on, &off, /*admin_password_configured=*/true,
                                          /*admin_pin_configured=*/false, &clear_web, &clear_lcd);
    TEST_CHECK(r == WEB_AUTH_POLICY_TRANSITION_OK, "disabling web auth must succeed");
    TEST_CHECK(clear_web == false, "disabling web auth must NOT clear sessions -- they are kept");

    // NULL guard: never a valid call, but must never report OK or write
    // through NULL out-params.
    r = web_auth_policy_check_transition(NULL, &web_on, true, true, NULL, NULL);
    TEST_CHECK(r != WEB_AUTH_POLICY_TRANSITION_OK, "a NULL current policy must never resolve OK");
}

// --- Item 10: the physical reset's real entry point -----------------------
// web_auth_store_clear_for_physical_reset() -- the header had no clear entry
// point for the administrator credential before this task; it was added
// here (see web_auth_store.h's doc comment on the function) rather than
// reached around from auth_reset_gesture.c, which must never open the
// kiln_auth namespace itself (check_kiln_auth_config_isolation.ps1's
// allowlist names only web_auth_store.c/.h). Forward fix: this function
// used to also erase the policy record back to ABSENT, which plan section
// 10 forbids ("does not disable authentication ... does not clear any
// config") -- it now leaves the policy completely untouched, so the tests
// below assert the policy survives byte-for-byte rather than going ABSENT.
static void test_clear_for_physical_reset(void)
{
    TEST_SECTION("web_auth_store_clear_for_physical_reset -- administrator password AND "
                 "administrator LCD PIN cleared, user credentials/policy untouched");
    reset_all();

    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_USER, "operator", "UserPassword1",
                                            SALT_A, false) == HAL_OK,
               "set up: user password configured");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", "AdminPass123",
                                            SALT_B, false) == HAL_OK,
               "set up: administrator password configured");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, "1234", SALT_A) == HAL_OK,
               "set up: user PIN configured");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "5678", SALT_B) == HAL_OK,
               "set up: administrator PIN configured");
    web_auth_policy_t pol = {.web_enabled = true, .lcd_enabled = true,
                              .web_timeout_s = 300, .lcd_timeout_s = 60};
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "set up: policy enabled and persisted");

    TEST_CHECK(web_auth_store_clear_for_physical_reset() == true,
               "the physical reset entry point reports success");

    web_auth_password_record_t admin_rec;
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_rec) ==
                   WEB_AUTH_LOAD_OK,
               "administrator record still loads OK (well-formed, just cleared)");
    TEST_CHECK(admin_rec.configured == false,
               "administrator no longer has a password configured after reset");
    TEST_CHECK(admin_rec.must_change == true,
               "administrator record's must_change is set true by the reset");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "AdminPass123") ==
                   false,
               "the old administrator password no longer verifies");

    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "UserPassword1") == true,
               "the user's password is untouched by an administrator-only reset");

    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, "1234") == true,
               "the user's LCD PIN is untouched by the credential reset");

    // Item 4a fix (2026-09-17 adversarial review, 1179e2d3): this used to
    // assert the OPPOSITE -- that the administrator's LCD PIN survived a
    // physical reset untouched. That was the bug: the documented physical
    // four-corner reset gesture exists to recover a forgotten credential,
    // and a forgotten LCD PIN is exactly the situation it must be able to
    // recover from. The administrator's LCD PIN record is now cleared
    // alongside the web password.
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "5678") == false,
               "the administrator's LCD PIN no longer verifies after the physical reset -- "
               "a forgotten LCD PIN is exactly the failure this gesture must recover from");
    TEST_CHECK(web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false,
               "administrator LCD PIN reads as not-configured after the reset");
    web_auth_pin_record_t admin_pin_rec;
    TEST_CHECK(web_auth_store_load_pin(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_pin_rec) ==
                   WEB_AUTH_LOAD_OK,
               "administrator LCD PIN record still loads OK (well-formed, just cleared) -- "
               "the LCD blob as a whole is not corrupted by the reset");

    web_auth_policy_t loaded_pol;
    TEST_CHECK(web_auth_store_load_policy(&loaded_pol) == WEB_AUTH_LOAD_OK,
               "policy record is left exactly as the owner set it -- plan section 10 "
               "forbids clearing any config, including policy");
    TEST_CHECK(loaded_pol.web_enabled == true && loaded_pol.lcd_enabled == true,
               "policy fields are byte-for-byte what was seeded before the reset");
    TEST_CHECK(web_auth_policy_effective_enabled(WEB_AUTH_LOAD_OK, loaded_pol.web_enabled) ==
                   true,
               "auth stays enabled after the reset -- this is the new reachable state "
               "(enabled, no administrator credential configured) that section 11's "
               "login-path work must treat as a forced set-a-new-password flow");
}

static void test_clear_for_physical_reset_on_empty_store(void)
{
    TEST_SECTION("web_auth_store_clear_for_physical_reset -- safe against a never-written store");
    reset_all();

    TEST_CHECK(web_auth_store_clear_for_physical_reset() == true,
               "clearing an already-empty store still reports success");
    web_auth_policy_t loaded_pol;
    TEST_CHECK(web_auth_store_load_policy(&loaded_pol) == WEB_AUTH_LOAD_ABSENT,
               "policy is (still) ABSENT -- untouched, and it was never written");
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false,
               "administrator still reads as not configured");
    TEST_CHECK(web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false,
               "administrator LCD PIN still reads as not configured (item 4a: the LCD blob "
               "clear is exercised even when it was never written)");
}

// web_auth_store_clear_all_credentials() -- item 12b's "Clear login
// credentials" ADMIN route, distinct from both the physical reset gesture
// above (administrator-only) and a factory reset (never touches
// credentials at all): this clears BOTH roles' web passwords AND both
// roles' LCD PINs, and leaves the policy record completely untouched.
static void test_clear_all_credentials(void)
{
    TEST_SECTION("web_auth_store_clear_all_credentials -- both roles' passwords AND PINs "
                 "cleared, policy untouched");
    reset_all();

    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_USER, "operator", "UserPassword1",
                                            SALT_A, false) == HAL_OK,
               "set up: user password configured");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", "AdminPass123",
                                            SALT_B, false) == HAL_OK,
               "set up: administrator password configured");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, "1234", SALT_A) == HAL_OK,
               "set up: user PIN configured");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "5678", SALT_B) == HAL_OK,
               "set up: administrator PIN configured");
    web_auth_policy_t pol = {.web_enabled = true, .lcd_enabled = true,
                              .web_timeout_s = 300, .lcd_timeout_s = 60};
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "set up: policy enabled and persisted");

    TEST_CHECK(web_auth_store_clear_all_credentials() == true,
               "clear_all_credentials reports success (every write confirmed by read-back)");

    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "AdminPass123") == false,
               "the old administrator password no longer verifies");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "UserPassword1") == false,
               "the old user password no longer verifies -- unlike the physical reset, the "
               "user record IS cleared by this action");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "5678") == false,
               "the old administrator LCD PIN no longer verifies");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, "1234") == false,
               "the old user LCD PIN no longer verifies");

    web_auth_password_record_t admin_rec;
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_rec) ==
                   WEB_AUTH_LOAD_OK,
               "administrator record still loads OK (well-formed, just cleared)");
    TEST_CHECK(admin_rec.configured == false, "administrator no longer configured");
    TEST_CHECK(admin_rec.must_change == true,
               "administrator record's must_change is set true, same as the physical reset");

    web_auth_password_record_t user_rec;
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_USER, &user_rec) == WEB_AUTH_LOAD_OK,
               "user record still loads OK (well-formed, just cleared)");
    TEST_CHECK(user_rec.configured == false, "user no longer configured");

    web_auth_policy_t loaded_pol;
    TEST_CHECK(web_auth_store_load_policy(&loaded_pol) == WEB_AUTH_LOAD_OK,
               "policy record is left exactly as the owner set it -- this action clears "
               "credentials only, never any config");
    TEST_CHECK(loaded_pol.web_enabled == true && loaded_pol.lcd_enabled == true &&
                   loaded_pol.web_timeout_s == 300 && loaded_pol.lcd_timeout_s == 60,
               "policy fields are byte-for-byte what was seeded before the clear");
}

static void test_clear_all_credentials_on_empty_store(void)
{
    TEST_SECTION("web_auth_store_clear_all_credentials -- safe against a never-written store");
    reset_all();

    TEST_CHECK(web_auth_store_clear_all_credentials() == true,
               "clearing an already-empty store still reports success");
    web_auth_policy_t loaded_pol;
    TEST_CHECK(web_auth_store_load_policy(&loaded_pol) == WEB_AUTH_LOAD_ABSENT,
               "policy is (still) ABSENT -- untouched, and it was never written");
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false &&
                   web_auth_store_password_configured(WEB_AUTH_ROLE_USER) == false,
               "both roles still read as not configured");
    TEST_CHECK(web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR) == false &&
                   web_auth_store_pin_configured(WEB_AUTH_ROLE_USER) == false,
               "both roles' LCD PINs still read as not configured");
}

// --- The WIRED gesture path: a real, non-null clear_credentials_fn --------
// Coordinator instruction: keep AUTH_RESET_CONFIRM_NOT_WIRED covered
// (test_auth_reset_gesture.c, unchanged) AND extend the negative cases to
// this wired path -- an aborted gesture must leave the stored credentials
// intact, not partially cleared.

static void seed_credentials_for_gesture_test(void)
{
    reset_all();
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_USER, "operator", "UserPassword1",
                                            SALT_A, false) == HAL_OK,
               "seed: user password configured");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", "AdminPass123",
                                            SALT_B, false) == HAL_OK,
               "seed: administrator password configured");
    web_auth_policy_t pol = {.web_enabled = true, .lcd_enabled = false,
                              .web_timeout_s = 300, .lcd_timeout_s = -1};
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "seed: policy persisted");
}

static void assert_credentials_untouched(const char *why)
{
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "AdminPass123") == true,
               why);
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "UserPassword1") == true, why);
    web_auth_policy_t loaded_pol;
    TEST_CHECK(web_auth_store_load_policy(&loaded_pol) == WEB_AUTH_LOAD_OK, why);
    TEST_CHECK(loaded_pol.web_enabled == true, why);
}

static void test_wired_gesture_full_confirm_clears_administrator_only(void)
{
    TEST_SECTION("wired gesture -- a full, confirmed gesture clears the administrator "
                 "through the real store, user and policy untouched");
    seed_credentials_for_gesture_test();

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    s.clear_credentials_fn = web_auth_store_clear_for_physical_reset;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, true, false, false);
    auth_reset_gesture_tap_result_t r4 = auth_reset_gesture_on_corner_tap(
        &s, AUTH_RESET_CORNER_BOTTOM_RIGHT, 1300, true, false, false);
    TEST_CHECK(r4 == AUTH_RESET_TAP_ARMED, "full correct sequence arms");

    auth_reset_gesture_confirm_result_t cr = auth_reset_gesture_confirm(&s, 1400);
    TEST_CHECK(cr == AUTH_RESET_CONFIRM_OK, "confirm through the real wired store -- OK");

    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "AdminPass123") ==
                   false,
               "administrator's old password no longer verifies after a real confirmed reset");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "UserPassword1") == true,
               "user's password survives a real confirmed reset");
    web_auth_policy_t loaded_pol;
    TEST_CHECK(web_auth_store_load_policy(&loaded_pol) == WEB_AUTH_LOAD_OK,
               "policy still reads back OK (not erased) after a real confirmed reset");
    TEST_CHECK(loaded_pol.web_enabled == true,
               "policy is left exactly as seeded -- auth stays enabled, satisfying plan "
               "section 10's 'does not disable authentication' requirement");
}

static void test_wired_gesture_out_of_order_leaves_store_intact(void)
{
    TEST_SECTION("wired gesture -- an out-of-order tap aborts and never touches the real store");
    seed_credentials_for_gesture_test();

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    s.clear_credentials_fn = web_auth_store_clear_for_physical_reset;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_tap_result_t r2 = auth_reset_gesture_on_corner_tap(
        &s, AUTH_RESET_CORNER_BOTTOM_RIGHT, 1100, true, false, false);
    TEST_CHECK(r2 == AUTH_RESET_TAP_ABANDONED, "out-of-order tap abandons the sequence");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 1200) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm after an abandoned sequence -- NOT_ARMED, the real seam is never called");

    assert_credentials_untouched(
        "an out-of-order tap must leave every real, wired credential and the policy intact");
}

static void test_wired_gesture_missing_corner_leaves_store_intact(void)
{
    TEST_SECTION("wired gesture -- a missing corner (only 3 of 4) never touches the real store");
    seed_credentials_for_gesture_test();

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    s.clear_credentials_fn = web_auth_store_clear_for_physical_reset;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, true, false, false);
    /* Never tap BOTTOM_RIGHT. */
    TEST_CHECK(!s.armed, "three of four taps -- never armed");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 1300) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm with a missing corner -- NOT_ARMED");

    assert_credentials_untouched(
        "a missing corner must leave every real, wired credential and the policy intact");
}

static void test_wired_gesture_estop_released_mid_sequence_leaves_store_intact(void)
{
    TEST_SECTION("wired gesture -- E-stop released mid-sequence never touches the real store");
    seed_credentials_for_gesture_test();

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    s.clear_credentials_fn = web_auth_store_clear_for_physical_reset;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    /* E-stop released before the third tap. */
    auth_reset_gesture_tap_result_t r = auth_reset_gesture_on_corner_tap(
        &s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, false, false, false);
    TEST_CHECK(r == AUTH_RESET_TAP_IGNORED_PRECONDITIONS,
               "tap while E-stop released -- ignored, sequence abandoned");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 1300) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm after E-stop was released mid-sequence -- NOT_ARMED");

    assert_credentials_untouched(
        "E-stop released mid-sequence must leave every real, wired credential and the "
        "policy intact");
}

static void test_wired_gesture_confirm_window_expired_leaves_store_intact(void)
{
    TEST_SECTION("wired gesture -- an expired confirm never touches the real store");
    seed_credentials_for_gesture_test();

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    s.clear_credentials_fn = web_auth_store_clear_for_physical_reset;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, true, false, false);
    auth_reset_gesture_tap_result_t r4 = auth_reset_gesture_on_corner_tap(
        &s, AUTH_RESET_CORNER_BOTTOM_RIGHT, 1300, true, false, false);
    TEST_CHECK(r4 == AUTH_RESET_TAP_ARMED, "full sequence arms");

    uint32_t too_late = 1300 + AUTH_RESET_GESTURE_CONFIRM_WINDOW_MS + 1;
    TEST_CHECK(auth_reset_gesture_confirm(&s, too_late) == AUTH_RESET_CONFIRM_EXPIRED,
               "confirm past the 30 s window -- EXPIRED, the real seam is never called");

    assert_credentials_untouched(
        "an expired confirm must leave every real, wired credential and the policy intact");
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
    test_policy_check_transition();
    test_clear_for_physical_reset();
    test_clear_for_physical_reset_on_empty_store();
    test_clear_all_credentials();
    test_clear_all_credentials_on_empty_store();
    test_wired_gesture_full_confirm_clears_administrator_only();
    test_wired_gesture_out_of_order_leaves_store_intact();
    test_wired_gesture_missing_corner_leaves_store_intact();
    test_wired_gesture_estop_released_mid_sequence_leaves_store_intact();
    test_wired_gesture_confirm_window_expired_leaves_store_intact();

    fake_kv_reset_all();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
