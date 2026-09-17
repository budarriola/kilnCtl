// Host tests for App/drivers/ui/lcd_credential_bridge.c -- this file is not
// thin glue: it carries two decisions of its own that nothing else tests --
// the try-administrator-then-user role ordering in
// lcd_credential_verify_pin(), and the ABSENT/OK/UNREADABLE -> effective
// "enabled" collapse in lcd_credential_load_policy(). web_auth_store.c's own
// host tests (test_web_auth_store.c) cover web_auth_store_verify_pin()/
// web_auth_policy_effective_enabled() in isolation, but not how this file
// combines them. Standalone executable (own g_test_failures/g_test_count),
// same direct-#include-the-module-under-test convention as
// test_web_auth_store.c, so this module's real logic runs against a real
// fake_kv-backed web_auth_store rather than a mirrored copy.
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "fake_kv.h"
#include "psa/crypto.h"

// Pulled in directly (not left to transitive inclusion via
// lcd_credential_bridge.c below) so lcd_pin_verify_fn_t/
// ui_lcd_lock_policy_fn_t are visible before the test-local stub
// definitions that use them, further down this file, are declared.
#include "../drivers/ui/lcd_auth_state.h"
#include "../drivers/ui/ui_lcd_lock.h"

int g_test_failures = 0;
int g_test_count = 0;

// psa/crypto.h's host stub declares this `extern` -- this standalone
// executable needs its own definition since it does not link
// test_ota_http.c.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "../drivers/persist/web_auth_store.c"

// lcd_credential_bridge.c calls lcd_auth_state_set_verify_fn() (real impl in
// lcd_auth_state.c, host-testable) and ui_lcd_lock_set_policy_fn() (real impl
// in ui_lcd_lock.c, LVGL-dependent, NOT host-testable). Since the linker must
// resolve every external symbol a linked translation unit references
// regardless of whether the referencing function is ever called, and this
// test never calls lcd_credential_bridge_init() itself, minimal test-local
// stubs stand in for both real setters rather than pulling in either real
// module (avoiding LVGL entirely, and avoiding a second definition of
// anything web_auth_store.c already gives us above).
static lcd_pin_verify_fn_t s_stub_verify_fn = NULL;
void lcd_auth_state_set_verify_fn(lcd_pin_verify_fn_t fn)
{
    s_stub_verify_fn = fn;
}

static ui_lcd_lock_policy_fn_t s_stub_policy_fn = NULL;
void ui_lcd_lock_set_policy_fn(ui_lcd_lock_policy_fn_t fn)
{
    s_stub_policy_fn = fn;
}

#include "../drivers/ui/lcd_credential_bridge.c"

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL);
}

static const uint8_t SALT_ADMIN[WEB_AUTH_SALT_LEN] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t SALT_USER[WEB_AUTH_SALT_LEN] = {
    16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

// --- lcd_credential_bridge_init() actually installs this file's functions,
// not something else -- covers the wiring itself, not just the logic. ------
static void test_init_installs_seams(void)
{
    reset_all();
    s_stub_verify_fn = NULL;
    s_stub_policy_fn = NULL;
    lcd_credential_bridge_init();
    TEST_CHECK(s_stub_verify_fn == lcd_credential_verify_pin,
               "init installs lcd_credential_verify_pin as the keypad seam");
    TEST_CHECK(s_stub_policy_fn == lcd_credential_load_policy,
               "init installs lcd_credential_load_policy as the lock seam");
}

// --- Role ordering: administrator PIN grants ADMIN, user PIN grants USER --
// Negative-test target: swap the two `return LCD_PIN_ROLE_*` labels inside
// lcd_credential_verify_pin() (a mislabeling bug distinct from a reordering
// bug, since with two distinct PINs only one of the two `if` branches can
// ever fire -- the label attached to that branch is the only thing that can
// actually flip which role gets granted).
static void test_role_ordering(void)
{
    reset_all();
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "9999", SALT_ADMIN) == HAL_OK,
               "seed administrator PIN");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, "1234", SALT_USER) == HAL_OK,
               "seed user PIN");

    TEST_CHECK(lcd_credential_verify_pin("9999", 4) == LCD_PIN_ROLE_ADMIN,
               "administrator PIN grants LCD_PIN_ROLE_ADMIN");
    TEST_CHECK(lcd_credential_verify_pin("1234", 4) == LCD_PIN_ROLE_USER,
               "user PIN grants LCD_PIN_ROLE_USER");
    TEST_CHECK(lcd_credential_verify_pin("0000", 4) == LCD_PIN_ROLE_NONE,
               "unrecognised PIN grants no role");
}

// --- UNREADABLE forces the lock on, regardless of the stored flag ---------
// Same direct-blob-mutation technique test_web_auth_store.c uses to
// construct an UNREADABLE record: write a normal record via the real
// setter, then bump its version past what this build understands and
// recompute the CRC over just that mutation, so only the version looks
// "from the future" (an OTA-rollback-past-a-schema-bump record, item 12b).
// Negative-test target: change lcd_credential_load_policy()'s
// `web_auth_policy_effective_enabled(status, stored.lcd_enabled)` to the raw
// `stored.lcd_enabled` -- an UNREADABLE record with lcd_enabled left zeroed
// by a failed load must no longer fail closed, and this test must go red.
static void test_unreadable_forces_enabled(void)
{
    reset_all();
    web_auth_policy_t seed = {
        .web_enabled = true,
        .lcd_enabled = false, // deliberately OFF in the stored flag
        .web_timeout_s = 900,
        .lcd_timeout_s = 300,
    };
    TEST_CHECK(web_auth_store_set_policy(&seed) == HAL_OK, "seed policy record");

    web_auth_policy_blob_t blob;
    TEST_CHECK(load_blob(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob)) == HAL_OK,
               "load raw policy blob for mutation");
    blob.version = WEB_AUTH_STORE_VERSION + 1;
    blob.crc32 = policy_blob_crc(&blob);
    TEST_CHECK(set_blob_verified(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob)) == HAL_OK,
               "write back mutated (future-versioned) policy blob");

    ui_lcd_lock_policy_t p = lcd_credential_load_policy();
    TEST_CHECK(p.enabled == true,
               "UNREADABLE policy record forces the LCD lock enabled, "
               "even though the raw stored flag was false");
}

// --- Timeout mapping: <= 0 (including -1 "never") maps to
// LCD_LOCK_TIMEOUT_NEVER; a positive value passes through unchanged. -------
// Negative-test target: change the `<= 0` boundary in
// lcd_credential_load_policy() to `< 0`, so a non-"-1" non-positive value
// (0, or an unexpected negative like -5) no longer maps to NEVER.
static void test_timeout_mapping(void)
{
    reset_all();
    web_auth_policy_t pol = {
        .web_enabled = true, .lcd_enabled = true,
        .web_timeout_s = 900, .lcd_timeout_s = -1,
    };
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "seed lcd_timeout_s = -1");
    TEST_CHECK(lcd_credential_load_policy().timeout_s == LCD_LOCK_TIMEOUT_NEVER,
               "-1 (never) maps to LCD_LOCK_TIMEOUT_NEVER");

    pol.lcd_timeout_s = -5; // any value at or below zero, not just -1
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "seed lcd_timeout_s = -5");
    TEST_CHECK(lcd_credential_load_policy().timeout_s == LCD_LOCK_TIMEOUT_NEVER,
               "any negative value below -1 also maps to LCD_LOCK_TIMEOUT_NEVER");

    pol.lcd_timeout_s = 0;
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "seed lcd_timeout_s = 0");
    TEST_CHECK(lcd_credential_load_policy().timeout_s == LCD_LOCK_TIMEOUT_NEVER,
               "zero maps to LCD_LOCK_TIMEOUT_NEVER");

    pol.lcd_timeout_s = 300;
    TEST_CHECK(web_auth_store_set_policy(&pol) == HAL_OK, "seed lcd_timeout_s = 300");
    TEST_CHECK(lcd_credential_load_policy().timeout_s == 300u,
               "a positive value passes through unchanged");
}

int main(void)
{
    test_init_installs_seams();
    test_role_ordering();
    test_unreadable_forces_enabled();
    test_timeout_mapping();

    fake_kv_reset_all();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
