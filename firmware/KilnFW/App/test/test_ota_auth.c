// Host tests for App/drivers/net/ota_auth.c -- UPDATE_PROTOCOL.md section 2's
// nonce lifecycle, constant-time compare, and lockout/backoff. No ESP-IDF
// dependency.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/ota_auth.h"

static void test_nonce_lifecycle(void)
{
    TEST_SECTION("ota_auth_nonce -- lifecycle");

    ota_auth_nonce_state_t s;
    memset(&s, 0, sizeof(s));

    TEST_CHECK(ota_auth_nonce_check(&s, 0) == OTA_AUTH_NONCE_NOT_ISSUED,
               "never-issued state reports NOT_ISSUED, not a false OK");

    uint8_t rand1[OTA_AUTH_NONCE_LEN];
    for (int i = 0; i < OTA_AUTH_NONCE_LEN; i++) {
        rand1[i] = (uint8_t)(i + 1);
    }
    ota_auth_nonce_issue(&s, rand1, 1000);
    TEST_CHECK(ota_auth_nonce_check(&s, 1000) == OTA_AUTH_NONCE_OK, "freshly issued -- OK immediately");
    TEST_CHECK(memcmp(s.nonce, rand1, OTA_AUTH_NONCE_LEN) == 0, "stored nonce matches what was issued");

    TEST_CHECK(ota_auth_nonce_check(&s, 1000 + OTA_AUTH_NONCE_EXPIRY_MS) == OTA_AUTH_NONCE_OK,
               "exactly at the expiry boundary is still OK (inclusive)");
    TEST_CHECK(ota_auth_nonce_check(&s, 1000 + OTA_AUTH_NONCE_EXPIRY_MS + 1) == OTA_AUTH_NONCE_EXPIRED,
               "one ms past the boundary -- expired");

    ota_auth_nonce_invalidate(&s);
    TEST_CHECK(ota_auth_nonce_check(&s, 1000) == OTA_AUTH_NONCE_ALREADY_USED,
               "invalidated nonce reports ALREADY_USED even though still within its window");

    // Idempotent invalidate.
    ota_auth_nonce_invalidate(&s);
    TEST_CHECK(ota_auth_nonce_check(&s, 1000) == OTA_AUTH_NONCE_ALREADY_USED,
               "double-invalidate is a harmless no-op, still ALREADY_USED");

    // A fresh issue overwrites/retires the old one, even mid-window and even
    // if the old one was never explicitly invalidated.
    uint8_t rand2[OTA_AUTH_NONCE_LEN];
    for (int i = 0; i < OTA_AUTH_NONCE_LEN; i++) {
        rand2[i] = (uint8_t)(0x80 + i);
    }
    ota_auth_nonce_issue(&s, rand2, 2000);
    TEST_CHECK(ota_auth_nonce_check(&s, 2000) == OTA_AUTH_NONCE_OK, "re-issued nonce is fresh/usable");
    TEST_CHECK(memcmp(s.nonce, rand2, OTA_AUTH_NONCE_LEN) == 0, "stored nonce is the NEW one, not rand1");
}

static void test_constant_time_equal(void)
{
    TEST_SECTION("ota_auth_constant_time_equal");

    uint8_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    TEST_CHECK(ota_auth_constant_time_equal(a, b, 8), "identical buffers compare equal");

    uint8_t c[8] = { 1, 2, 3, 4, 5, 6, 7, 9 }; // differs only in the LAST byte
    TEST_CHECK(!ota_auth_constant_time_equal(a, c, 8),
               "difference in the last byte is still detected (not just early-exit-checked)");

    uint8_t d[8] = { 9, 2, 3, 4, 5, 6, 7, 8 }; // differs only in the FIRST byte
    TEST_CHECK(!ota_auth_constant_time_equal(a, d, 8), "difference in the first byte is detected");

    uint8_t zero_len_a[1] = { 0 };
    uint8_t zero_len_b[1] = { 0 };
    TEST_CHECK(ota_auth_constant_time_equal(zero_len_a, zero_len_b, 0),
               "zero-length compare is vacuously true");
}

static void test_lockout(void)
{
    TEST_SECTION("ota_auth_lockout -- threshold, doubling backoff, reset on success");

    ota_auth_lockout_state_t s;
    memset(&s, 0, sizeof(s));

    TEST_CHECK(!ota_auth_lockout_is_locked(&s, 0), "fresh state is not locked");

    ota_auth_lockout_record_failure(&s, 0);
    ota_auth_lockout_record_failure(&s, 1);
    TEST_CHECK(!ota_auth_lockout_is_locked(&s, 1), "two failures -- not locked yet (threshold is 3)");

    ota_auth_lockout_record_failure(&s, 2); // 3rd failure -- triggers the first lockout
    TEST_CHECK(ota_auth_lockout_is_locked(&s, 2), "third failure locks immediately");
    TEST_CHECK(ota_auth_lockout_is_locked(&s, 2 + OTA_AUTH_LOCKOUT_BASE_MS - 1),
               "still locked one ms before the 60s window elapses");
    TEST_CHECK(!ota_auth_lockout_is_locked(&s, 2 + OTA_AUTH_LOCKOUT_BASE_MS),
               "unlocked exactly when the 60s window elapses");

    // Second lockout cycle (another 3 failures after the first lock expired)
    // -- backoff should double to 120s.
    uint32_t t = 2 + OTA_AUTH_LOCKOUT_BASE_MS;
    ota_auth_lockout_record_failure(&s, t);
    ota_auth_lockout_record_failure(&s, t + 1);
    ota_auth_lockout_record_failure(&s, t + 2); // triggers second lockout
    TEST_CHECK(ota_auth_lockout_is_locked(&s, t + 2), "second lockout cycle locks again");
    TEST_CHECK(!ota_auth_lockout_is_locked(&s, t + 2 + 2u * OTA_AUTH_LOCKOUT_BASE_MS),
               "second lockout lifts after 120s (60s * 2^1), not 60s again");
    TEST_CHECK(ota_auth_lockout_is_locked(&s, t + 2 + 2u * OTA_AUTH_LOCKOUT_BASE_MS - 1),
               "still locked one ms before the doubled window elapses");

    // Drive enough additional lockout cycles to confirm the ceiling holds.
    ota_auth_lockout_state_t ceil_s;
    memset(&ceil_s, 0, sizeof(ceil_s));
    uint32_t now = 0;
    for (int cycle = 0; cycle < 10; cycle++) {
        ota_auth_lockout_record_failure(&ceil_s, now);
        ota_auth_lockout_record_failure(&ceil_s, now + 1);
        ota_auth_lockout_record_failure(&ceil_s, now + 2);
        now = ceil_s.locked_until_ms; // jump to exactly when this lock lifts
    }
    TEST_CHECK(ceil_s.locked_until_ms > 0, "still produces a real lockout after many cycles");
    // The most recent lock's duration must never exceed OTA_AUTH_LOCKOUT_MAX_MS.
    uint32_t last_lock_start = now; // 'now' was set to the PREVIOUS locked_until_ms before this
                                     // cycle's failures ran, so it's this lock's start time
    TEST_CHECK((ceil_s.locked_until_ms - last_lock_start) <= OTA_AUTH_LOCKOUT_MAX_MS,
               "backoff duration never exceeds the 15-minute ceiling, even after many escalations");

    // Reset on success.
    ota_auth_lockout_record_success(&s);
    TEST_CHECK(!ota_auth_lockout_is_locked(&s, t + 2), "success immediately clears an active lock");
    ota_auth_lockout_record_failure(&s, t + 100);
    ota_auth_lockout_record_failure(&s, t + 101);
    ota_auth_lockout_record_failure(&s, t + 102); // 3rd failure after reset -- back to tier 0
    TEST_CHECK((s.locked_until_ms - (t + 102)) == OTA_AUTH_LOCKOUT_BASE_MS,
               "after a success reset, the next lockout starts back at the base 60s, not the "
               "escalated tier from before");
}

void run_test_ota_auth(void)
{
    test_nonce_lifecycle();
    test_constant_time_equal();
    test_lockout();
}
